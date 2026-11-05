// Browser-only regression checks for the worker-to-main WebGPU readback bridge.
#include "../webgpu/webgpu_device.h"

#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
std::unique_ptr<gpu::WebGpuDevice> device;
int checks = 0, failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)

// Odd widths and padded destinations catch both the GPU's 256-byte row
// alignment and accidental copying of staging padding into caller memory.
void texture_rows(gpu::Format format, int bpp) {
    auto &d = *device;
    auto t = d.create_texture({7, 5, format, gpu::UsageSampled | gpu::UsageRenderTarget});
    CHECK(t);
    std::vector<uint8_t> input(7 * 5 * bpp), output(5 * (7 * bpp + 9), 0xcd);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = uint8_t(i * 17 + 3);
    for (int round = 0; round < 3; ++round) {
        input[0] = uint8_t(round + 7);
        CHECK(d.upload(t, {0, 0, 7, 5}, input.data(), 7 * bpp));
        CHECK(d.readback(t, {0, 0, 7, 5}, output.data(), 7 * bpp + 9));
        for (int y = 0; y < 5; ++y) {
            CHECK(memcmp(output.data() + y * (7 * bpp + 9), input.data() + y * 7 * bpp, 7 * bpp) ==
                  0);
            for (int x = 0; x < 9; ++x)
                CHECK(output[y * (7 * bpp + 9) + 7 * bpp + x] == 0xcd);
        }
    }
    std::vector<uint8_t> region(3 * 2 * bpp);
    CHECK(d.readback(t, {2, 1, 3, 2}, region.data(), 3 * bpp));
    for (int y = 0; y < 2; ++y)
        CHECK(memcmp(region.data() + y * 3 * bpp, input.data() + ((y + 1) * 7 + 2) * bpp,
                     3 * bpp) == 0);
    CHECK(!d.readback(t, {-1, 0, 1, 1}, output.data(), 7 * bpp));
    CHECK(!d.readback(t, {6, 0, 2, 1}, output.data(), 7 * bpp));
    CHECK(!d.readback(t, {0, 0, 7, 5}, output.data(), 7 * bpp - 1));
    CHECK(!d.readback(t, {0, 0, 0, 5}, output.data(), 7 * bpp));
    d.destroy(t);
    CHECK(!d.readback(t, {0, 0, 1, 1}, output.data(), 4));
}

void copied_buffers_and_completion() {
    auto &d = *device;
    auto t = d.create_texture({7, 3, gpu::Format::RGBA8, gpu::UsageRenderTarget});
    auto b = d.create_buffer(256 * 3, nullptr);
    CHECK(t && b);
    const uint8_t *first = nullptr;
    for (int round = 0; round < 3; ++round) {
        auto cb = d.begin();
        gpu::RenderPass pass;
        pass.color[0].texture = t;
        pass.color[0].load = gpu::Load::Clear;
        pass.color[0].clear[0] = round == 1 ? 1 : 0;
        pass.color[0].clear[1] = round == 2 ? 1 : 0;
        pass.color[0].clear[2] = round == 0 ? 1 : 0;
        pass.color[0].clear[3] = 1;
        d.begin_render_pass(cb, pass);
        d.end_render_pass(cb);
        d.copy_texture_to_buffer(cb, t, {0, 0, 7, 3}, b, 0, 256);
        bool called = false;
        d.on_complete(cb, [&](gpu::CommandStatus s, double) {
            called = true;
            CHECK(s == gpu::CommandStatus::Completed);
        });
        d.commit(cb);
        d.wait(cb);
        CHECK(called);
        CHECK(d.status(cb) == gpu::CommandStatus::Completed);
        auto *bytes = static_cast<const uint8_t *>(d.map_read(b));
        CHECK(bytes);
        if (!bytes)
            continue;
        // The returned storage survives browser unmap and is reused safely.
        if (first)
            CHECK(first == bytes);
        first = bytes;
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 7; ++x) {
                const uint8_t *p = bytes + y * 256 + x * 4;
                CHECK(p[0] == (round == 1 ? 255 : 0));
                CHECK(p[1] == (round == 2 ? 255 : 0));
                CHECK(p[2] == (round == 0 ? 255 : 0));
                CHECK(p[3] == 255);
            }
    }
    uint32_t marker = 0x12345678;
    d.update(b, 0, &marker, sizeof marker);
    auto *fresh = static_cast<const uint32_t *>(d.map_read(b));
    CHECK(fresh && fresh[0] == marker);
    d.destroy(b);
    CHECK(d.map_read(b) == nullptr);
    d.destroy(t);
}

void depth_checkpoint() {
    auto &d = *device;
    auto depth = d.create_texture({8, 4, gpu::Format::Depth32F, gpu::UsageRenderTarget});
    auto buffer = d.create_buffer(256 * 4, nullptr);
    auto cb = d.begin();
    gpu::RenderPass pass;
    pass.color_count = 0;
    pass.depth.texture = depth;
    pass.depth.load = gpu::Load::Clear;
    pass.depth.clear = 0.25;
    d.begin_render_pass(cb, pass);
    d.end_render_pass(cb);
    d.copy_texture_to_buffer(cb, depth, {0, 0, 8, 4}, buffer, 0, 256);
    d.commit(cb);
    // map_read must order its copy even without an explicit wait.
    auto *bytes = static_cast<const uint8_t *>(d.map_read(buffer));
    CHECK(bytes);
    if (bytes)
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 8; ++x) {
                float value;
                memcpy(&value, bytes + y * 256 + x * 4, 4);
                CHECK(value == 0.25f);
            }
    float values[32] = {};
    CHECK(d.readback(depth, {0, 0, 8, 4}, values, 8 * 4));
    for (float value : values)
        CHECK(value == 0.25f);
    float subregion[4] = {};
    CHECK(d.readback(depth, {3, 1, 2, 2}, subregion, 2 * 4));
    for (float value : subregion)
        CHECK(value == 0.25f);
    cb = d.begin();
    pass.depth.clear = 0.75;
    d.begin_render_pass(cb, pass);
    d.end_render_pass(cb);
    d.copy_buffer_to_texture(cb, buffer, 0, 256, depth, {0, 0, 8, 4});
    d.commit(cb);
    CHECK(d.readback(depth, {0, 0, 8, 4}, values, 8 * 4));
    for (float value : values)
        CHECK(value == 0.25f);
    const float patch[] = {0.1f, 0.2f, 0.3f, 0.4f};
    CHECK(d.upload(depth, {3, 1, 2, 2}, patch, 2 * 4));
    CHECK(d.readback(depth, {0, 0, 8, 4}, values, 8 * 4));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x)
            CHECK(values[y * 8 + x] ==
                  (x >= 3 && x < 5 && y >= 1 && y < 3 ? patch[(y - 1) * 2 + x - 3] : 0.25f));
    d.destroy(buffer);
    d.destroy(depth);
}

// Exercise both other Region-by-value proxy calls. Subrectangles must retain
// their coordinates, pitch and offsets across the worker/main boundary.
void uploaded_copy_and_blit() {
    auto &d = *device;
    std::vector<uint8_t> rows(256 * 2, 0);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3 * 4; ++x)
            rows[y * 256 + x] = uint8_t(10 + y * 32 + x);
    auto b = d.create_buffer(rows.size(), rows.data());
    auto src = d.create_texture({5, 4, gpu::Format::RGBA8, gpu::UsageSampled});
    auto dst = d.create_texture({6, 4, gpu::Format::RGBA8, gpu::UsageSampled});
    auto cb = d.begin();
    d.copy_buffer_to_texture(cb, b, 0, 256, src, {1, 1, 3, 2});
    d.blit(cb, src, {1, 1, 3, 2}, dst, 2, 1);
    d.commit(cb);
    uint8_t output[6 * 4 * 4] = {};
    CHECK(d.readback(dst, {0, 0, 6, 4}, output, 6 * 4));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 6; ++x)
            for (int c = 0; c < 4; ++c) {
                const uint8_t expected =
                    y >= 1 && y < 3 && x >= 2 && x < 5 ? rows[(y - 1) * 256 + (x - 2) * 4 + c] : 0;
                CHECK(output[(y * 6 + x) * 4 + c] == expected);
            }
    d.destroy(b);
    d.destroy(src);
    cb = d.begin();
    d.blit(cb, dst, {0, 0, 6, 4}, dst, 0, 0); // identical copy is a no-op
    d.blit(cb, dst, {2, 1, 3, 2}, dst, 1, 0); // overlapping self-copy
    d.commit(cb);
    CHECK(d.readback(dst, {0, 0, 6, 4}, output, 6 * 4));
    for (int y = 0; y < 2; ++y)
        CHECK(memcmp(output + (y * 6 + 1) * 4, rows.data() + y * 256, 3 * 4) == 0);
    d.destroy(dst);
}

// A rejected browser mapping must wake the waiting guest and return no stale
// bytes. Run last because destroying this device deliberately ends its use.
void lost_device_read() {
    auto b = device->create_buffer(16, nullptr);
    CHECK(b);
    emscripten_proxy_sync(
        emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
        [](void *) { wgpuDeviceDestroy(device->native_device()); }, nullptr);
    CHECK(device->map_read(b) == nullptr);
    device->destroy(b);
}

void run() {
    texture_rows(gpu::Format::RGBA8, 4);
    texture_rows(gpu::Format::BGRA8, 4);
    texture_rows(gpu::Format::R8, 1);
    copied_buffers_and_completion();
    depth_checkpoint();
    uploaded_copy_and_blit();
    lost_device_read();
    printf("WebGPU readback: %d checks, %d failures\n", checks, failures);
    MAIN_THREAD_EM_ASM(
        {
            window.gpuReadbackChecks = $0;
            window.gpuReadbackFailures = $1;
        },
        checks, failures);
}
} // namespace

int main() {
    device = gpu::WebGpuDevice::create();
    if (!device)
        return 1;
    std::thread(run).detach();
    return 0;
}
