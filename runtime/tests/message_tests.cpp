// Windows message queues belong to their receiving guest threads.
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include <cstdio>
#include <initializer_list>

static int checks, failures, host_ticks;
static uint32_t scratch, ready, proceed;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
        }                                                                                          \
    } while (0)
static uint32_t call(X86 *c, const char *dll, const char *name,
                     std::initializer_list<uint32_t> args = {}) {
    return guest_call(c, imports_resolve(dll, name), args.begin(), int(args.size()));
}
static void proc(X86 *c) {
    set_eax(c, arg(c, 1) == 0x81 ? 1 : 0);
}
static bool waiter() {
    ++host_ticks;
    return false;
}
static void quit_worker(X86 *c) {
    CHECK(call(c, "user32.dll", "PeekMessageA", {scratch + 0x100, 0, 0, 0, 1}) == 0);
    call(c, "user32.dll", "PostQuitMessage", {77});
    call(c, "kernel32.dll", "SetEvent", {ready});
    CHECK(call(c, "kernel32.dll", "WaitForSingleObject", {proceed, 1000}) == 0);
    CHECK(call(c, "user32.dll", "GetMessageA", {scratch + 0x100, 0, 0, 0}) == 0);
    CHECK(rd32(scratch + 0x104) == 0x12 && rd32(scratch + 0x108) == 77);
    set_eax(c, 0);
}
static void messages_worker(X86 *c) {
    call(c, "kernel32.dll", "SetEvent", {ready});
    CHECK(call(c, "user32.dll", "GetMessageW", {scratch + 0x100, 0, 0, 0}) == 1);
    CHECK(rd32(scratch + 0x104) == 0x8001 && rd32(scratch + 0x108) == 0x1234);
    CHECK(call(c, "user32.dll", "GetMessageW", {scratch + 0x100, 0, 0, 0}) == 0);
    CHECK(rd32(scratch + 0x108) == 99);
    set_eax(c, 0);
}
int main() {
    mem_init();
    imports_init();
    win32_init(".");
    X86 *c = loader_context();
    loader_init_context(c);
    sched_set_guest_thread(true);
    scratch = heap_alloc(0x1000, true);
    const uint32_t wndproc = imports_alloc_trampoline("test", "wndproc", proc, 4);
    gm_put_str(scratch + 0x800, "MessageTest", 32);
    wr32(scratch + 4, wndproc);
    wr32(scratch + 16, IMAGE_BASE);
    wr32(scratch + 36, scratch + 0x800);
    CHECK(call(c, "user32.dll", "RegisterClassA", {scratch}) != 0);
    uint32_t window = call(c, "user32.dll", "CreateWindowExA",
                           {0, scratch + 0x800, 0, 0, 0, 0, 8, 8, 0, 0, IMAGE_BASE, 0});
    CHECK(window != 0);
    // No helper may consume a key posted to the main thread's window.
    call(c, "user32.dll", "PostMessageA", {window, 0x100, 13, 0});
    ready = call(c, "kernel32.dll", "CreateEventA", {0, 0, 0, 0});
    proceed = call(c, "kernel32.dll", "CreateEventA", {0, 0, 0, 0});
    uint32_t entry = imports_alloc_trampoline("test", "quit_worker", quit_worker, 1);
    uint32_t thread = call(c, "kernel32.dll", "CreateThread", {0, 0, entry, 0, 0, 0});
    CHECK(call(c, "kernel32.dll", "WaitForSingleObject", {ready, 1000}) == 0);
    CHECK(call(c, "user32.dll", "PeekMessageA", {scratch + 0x200, window, 0x100, 0x100, 1}) == 1);
    CHECK(rd32(scratch + 0x208) == 13);
    CHECK(call(c, "user32.dll", "PeekMessageA", {scratch + 0x200, 0xffffffffu, 0, 0, 1}) == 0);
    call(c, "kernel32.dll", "SetEvent", {proceed});
    CHECK(call(c, "kernel32.dll", "WaitForSingleObject", {thread, 1000}) == 0);
    // A helper with an empty GetMessage queue yields without running the host.
    host_set_message_waiter(waiter);
    entry = imports_alloc_trampoline("test", "messages_worker", messages_worker, 1);
    thread = call(c, "kernel32.dll", "CreateThread", {0, 0, entry, 0, 0, scratch + 0x700});
    uint32_t id = rd32(scratch + 0x700);
    CHECK(call(c, "kernel32.dll", "WaitForSingleObject", {ready, 1000}) == 0);
    call(c, "kernel32.dll", "Sleep", {20});
    CHECK(call(c, "user32.dll", "PostThreadMessageA", {id, 0x8001, 0x1234, 0}) == 1);
    CHECK(call(c, "user32.dll", "PostThreadMessageW", {id, 0x12, 99, 0}) == 1);
    CHECK(call(c, "kernel32.dll", "WaitForSingleObject", {thread, 1000}) == 0);
    CHECK(host_ticks == 0);
    CHECK(call(c, "user32.dll", "PostThreadMessageA", {id, 0x8001, 0, 0}) == 0);
    CHECK(call(c, "kernel32.dll", "GetLastError") == 1444);
    // DefWindowProc's close posts quit to its calling thread.
    call(c, "user32.dll", "DefWindowProcA", {window, 0x10, 0, 0});
    CHECK(call(c, "user32.dll", "PeekMessageA", {scratch + 0x200, 0, 0x12, 0x12, 1}) == 1);
    CHECK(rd32(scratch + 0x204) == 0x12);
    host_set_message_waiter(nullptr);
    sched_set_guest_thread(false);
    std::printf("messages: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
