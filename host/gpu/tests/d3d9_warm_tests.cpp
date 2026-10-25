// Exercise interleavings with gates, without timing-dependent sleeps or a GPU.
#include "../d3d9_warm.h"
#include "../../../dx/host_d9.h"
#include <future>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

static int failures = 0, checks = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)

static void cache_tests() {
    d9warm::OnceCache<int> cache;
    d9warm::Pool no_workers(0);
    int builds = 0;
    cache.enqueue(no_workers, "queued", [&] {
        ++builds;
        return 42;
    });
    CHECK(cache.get("queued", [] { return -1; }) == 42); // draw claims queued work
    CHECK(builds == 1 && cache.stats.takeovers == 1);
    CHECK(cache.get("queued", [] { return -2; }) == 42 && builds == 1);

    d9warm::Pool pool(2);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    cache.enqueue(pool, "running", [&] {
        entered.set_value();
        gate.wait();
        return 91;
    });
    entered.get_future().wait();
    auto draw =
        std::async(std::launch::async, [&] { return cache.get("running", [] { return -3; }); });
    while (cache.stats.waits.load() < 1)
        std::this_thread::yield();
    // A different key can build while a worker and draw share the running key.
    CHECK(cache.get("independent", [] { return 7; }) == 7);
    release.set_value();
    CHECK(draw.get() == 91);
    CHECK(cache.stats.builds == 3);
    CHECK(!cache.get("throw", []() -> int { throw std::runtime_error("compile failure"); }));
    CHECK(!cache.get("throw", [] { return 8; }));
    CHECK(cache.stats.abandoned == 1);
    auto unwound = cache.reserve("unwind", []() -> int { throw 37; });
    try {
        (void)cache.get("unwind", [] { return 0; });
        CHECK(false);
    } catch (int value) {
        CHECK(value == 37);
    }
    CHECK(cache.state(unwound) == d9warm::OnceCache<int>::State::Abandoned);
    CHECK(!cache.get("unwind", [] { return 1; }));
    pool.stop();
    CHECK(!pool.submit([] {}));
}
static void abandonment_wakes_waiters() {
    d9warm::OnceCache<int> cache;
    d9warm::Pool pool(1);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    cache.enqueue(pool, "bad", [&]() -> int {
        entered.set_value();
        gate.wait();
        throw std::runtime_error("bad");
    });
    entered.get_future().wait();
    auto a = std::async(std::launch::async, [&] { return cache.get("bad", [] { return 1; }); });
    auto b = std::async(std::launch::async, [&] { return cache.get("bad", [] { return 2; }); });
    while (cache.stats.waits.load() < 2)
        std::this_thread::yield();
    release.set_value();
    CHECK(!a.get() && !b.get());
    CHECK(cache.stats.builds == 1 && cache.stats.abandoned == 1);
    pool.stop();
}
static void pool_tests() {
    d9warm::Pool pool(1);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    CHECK(pool.submit([&] {
        entered.set_value();
        gate.wait();
    }));
    entered.get_future().wait();
    std::atomic<int> calls = 0;
    for (unsigned i = 0; i < 10; ++i)
        CHECK(pool.submit([&] { ++calls; }));
    CHECK(pool.pending() == 10);
    auto stop = std::async(std::launch::async, [&] { pool.stop(); });
    // stop discards queued work before it waits for the running job.
    while (pool.pending())
        std::this_thread::yield();
    release.set_value();
    stop.get();
    CHECK(calls == 0 && pool.pending() == 0);
}
static d9warm::PipelineHint pipeline() {
    d9warm::PipelineHint p;
    p.vertex = 0x123;
    p.pixel = 0x456;
    p.variant = 0x100000000;
    p.colors[0] = 80;
    p.depth = 260;
    p.samples = 4;
    p.strides[0] = 16;
    p.frequencies[0] = HOST_D9_STREAM_INDEXED | 8;
    p.strides[6] = 16;
    p.frequencies[6] = HOST_D9_STREAM_INSTANCE | 2;
    p.states = {1, 5, 6, 1, 0, 2, 7, 3, 15, 15, 15, 15};
    p.declaration = {0, 0, 0, 0, 3, 0, 0, 0, 255, 0, 0, 0, 17, 0, 0, 0};
    return p;
}
static void hint_tests() {
    d9warm::ShaderHint s, shader{0x123, 0, false};
    CHECK(d9warm::parse_shader_hint(shader.line(), &s) && s.code == shader.code && !s.pixel);
    CHECK(!d9warm::parse_shader_hint("S1 123 2 0", &s));
    CHECK(!d9warm::parse_shader_hint("S1 123 0 1", &s));
    CHECK(!d9warm::parse_shader_hint("S1 123 1 0 extra", &s));
    CHECK(!d9warm::parse_shader_hint("S1 -1 1 0", &s));
    auto p = pipeline(), same = p;
    same.states[5] = 17;
    same.states[6] = 1;
    same.states[7] = 5;
    same.frequencies[0] = HOST_D9_STREAM_INDEXED | 200;
    CHECK(p.line() == same.line());
    same.states[4] = 1;
    CHECK(p.line() != same.line());
    same = p;
    same.frequencies[6] = HOST_D9_STREAM_INSTANCE | 1;
    CHECK(p.line() != same.line());
    same = p;
    same.strides[0] = 20;
    CHECK(p.line() != same.line());
    d9warm::PipelineHint decoded;
    CHECK(d9warm::parse_pipeline_hint(p.line(), &decoded));
    CHECK(decoded.line() == p.line() && decoded.frequencies[0] == 1);
    CHECK(!d9warm::parse_pipeline_hint(p.line() + " extra", &decoded));
    auto bad = p;
    bad.declaration.back() = 255; // valid bytes but invalid terminator stream next
    bad.declaration[bad.declaration.size() - 8] = 0;
    CHECK(!d9warm::parse_pipeline_hint(bad.line(), &decoded));
    bad = p;
    bad.states[0] = 2; // canonical serialization normalizes booleans
    bad.samples = 3;
    CHECK(!d9warm::parse_pipeline_hint(bad.line(), &decoded));

    std::string path = "build/d3d9-warm-tests-XXXXXX";
    int fd = os_mkstemp(path.data());
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    os_fd_close(fd);
    os_unlink(path.c_str());
    std::filesystem::create_directory(path);
    {
        d9warm::Hints hints(path);
        CHECK(hints.remember(shader));
        CHECK(hints.remember(p)); // draw reaches miss before queued worker
        CHECK(!hints.remember(same = p));
        CHECK(hints.flush());
    }
    {
        std::ofstream file(path + "/d3d9-warm.txt", std::ios::app | std::ios::binary);
        file << "P1 damaged\n" << std::string(10000, 'x') << "\n" << "S1 999 1 0\nS1 torn";
    }
    d9warm::Hints loaded(path);
    loaded.load();
    CHECK(loaded.damaged());
    CHECK(loaded.shaders().size() == 2 && loaded.pipelines().size() == 1);
    CHECK(loaded.flush() && !loaded.damaged());
    d9warm::Hints repaired(path);
    repaired.load();
    CHECK(!repaired.damaged() && repaired.shaders().size() == 2 &&
          repaired.pipelines().size() == 1);
    // Whole-line writes survive competing additions and flushes.
    auto writer = std::async(std::launch::async, [&] {
        for (uint64_t i = 1; i <= 100; ++i) {
            loaded.remember(d9warm::ShaderHint{i, 0, false});
            loaded.flush();
        }
    });
    for (uint64_t i = 101; i <= 200; ++i) {
        loaded.remember(d9warm::ShaderHint{i, 0, false});
        loaded.flush();
    }
    writer.get();
    CHECK(loaded.flush());
    d9warm::Hints concurrent(path);
    concurrent.load();
    CHECK(!concurrent.damaged() && concurrent.shaders().size() == 202);
    std::filesystem::remove_all(path);
}
int main() {
    cache_tests();
    abandonment_wakes_waiters();
    pool_tests();
    hint_tests();
    std::printf("d3d9 warm: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
