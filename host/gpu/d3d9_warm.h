// Portable scheduling, once-only builds and session hints for D3D9 warm-up.
// GPU objects and all backend-specific compilation stay in the backend.
#pragma once
#include "../../platform/os.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace d9warm {

class Pool {
  public:
    explicit Pool(unsigned threads = 2);
    ~Pool();
    bool submit(std::function<void()> job);
    void stop(); // Discard queued work and join running work before owners die.
    size_t pending() const;
    Pool(const Pool &) = delete;
    Pool &operator=(const Pool &) = delete;

  private:
    static void *worker(void *self);
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
    std::vector<OsThread *> workers_;
    bool stopping_ = false;
};

struct CacheStats {
    std::atomic<uint64_t> builds{0}, takeovers{0}, waits{0}, abandoned{0};
};

// A worker and a draw may race to claim a queued entry; exactly one wins.
// No map/entry lock is held while building or waiting on another entry.
// An exception leaves a terminal abandoned entry and wakes every waiter.
template <class T, class Key = std::string> class OnceCache {
  public:
    enum class State { Queued, Running, Ready, Abandoned };
    struct Entry {
        std::mutex mutex;
        std::condition_variable changed;
        State state = State::Queued;
        bool submitted = false;
        std::function<T()> builder;
        std::optional<T> value;
    };
    using Handle = std::shared_ptr<Entry>;
    Handle reserve(const Key &key, std::function<T()> builder) {
        std::lock_guard lock(mutex_);
        auto found = entries_.find(key);
        if (found != entries_.end())
            return found->second;
        auto entry = std::make_shared<Entry>();
        entry->builder = std::move(builder);
        entries_.emplace(key, entry);
        return entry;
    }
    void enqueue(Pool &pool, const Key &key, std::function<T()> builder) {
        const auto entry = reserve(key, std::move(builder));
        std::lock_guard lock(entry->mutex);
        if (entry->state != State::Queued || entry->submitted)
            return;
        entry->submitted = true;
        pool.submit([this, entry] { run(entry, false); });
    }
    std::optional<T> get(const Key &key, std::function<T()> builder) {
        auto entry = reserve(key, std::move(builder));
        run(entry, true);
        std::unique_lock lock(entry->mutex);
        if (entry->state == State::Running) {
            ++stats.waits;
            entry->changed.wait(lock, [&] { return entry->state != State::Running; });
        }
        return entry->state == State::Ready ? entry->value : std::nullopt;
    }
    State state(const Handle &entry) {
        std::lock_guard lock(entry->mutex);
        return entry->state;
    }
    CacheStats stats;

  private:
    void run(const Handle &entry, bool draw) {
        std::function<T()> build;
        {
            std::lock_guard lock(entry->mutex);
            if (entry->state != State::Queued)
                return;
            entry->state = State::Running;
            build = std::move(entry->builder);
        }
        ++stats.builds;
        if (draw)
            ++stats.takeovers;
        struct Finish {
            Handle entry;
            CacheStats *stats;
            bool committed = false;
            ~Finish() {
                if (!committed) {
                    std::lock_guard lock(entry->mutex);
                    entry->state = State::Abandoned;
                    ++stats->abandoned;
                }
                entry->changed.notify_all();
            }
        } finish{entry, &stats};
        try {
            T value = build();
            std::lock_guard lock(entry->mutex);
            entry->value = std::move(value);
            entry->state = State::Ready;
            finish.committed = true;
        } catch (const std::exception &) {
            // A failed native build is terminal for this session. It must
            // never strand a draw on an entry whose builder has unwound.
        }
    }
    std::mutex mutex_;
    std::unordered_map<Key, Handle> entries_;
};

struct ShaderHint {
    uint64_t code = 0, variant = 0;
    bool pixel = false;
    std::string line() const;
};

// Only inputs to pipeline creation belong here. Resource/declaration object
// ids, stream offsets and instance counts cannot change a compiled pipeline.
struct PipelineHint {
    uint64_t vertex = 0, pixel = 0, variant = 0;
    uint32_t samples = 1, depth = 0;
    std::array<uint32_t, 4> colors{};
    std::array<uint32_t, 8> strides{}, frequencies{};
    // Blend enable, RGB src/dst/op, separate-alpha enable, alpha src/dst/op,
    // then the four color write masks. Disabled factors are canonicalized.
    std::array<uint32_t, 12> states{};
    std::vector<uint8_t> declaration;
    PipelineHint canonical() const;
    std::string line() const;
};
bool parse_shader_hint(const std::string &line, ShaderHint *out);
bool parse_pipeline_hint(const std::string &line, PipelineHint *out);

class Hints {
  public:
    explicit Hints(std::string profile);
    void load();
    bool remember(const ShaderHint &hint);
    bool remember(const PipelineHint &hint);
    bool flush(); // Snapshot -> unique temporary file -> rename, whole lines.
    bool damaged() const;
    std::vector<ShaderHint> shaders() const;
    std::vector<PipelineHint> pipelines() const;

  private:
    bool remember_line(std::string line);
    std::string profile_;
    mutable std::mutex mutex_;
    std::mutex file_mutex_;
    std::set<std::string> lines_;
    bool dirty_ = false, damaged_ = false;
    uint64_t revision_ = 0;
};

} // namespace d9warm
