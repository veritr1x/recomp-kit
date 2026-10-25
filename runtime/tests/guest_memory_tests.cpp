// Guest addresses must fault above the arena, including multi-byte accesses
// crossing the highest 32-bit address. Run faults in subprocesses so the
// parent can distinguish a real access violation from a successful read.
#if defined(TEST_GUEST_SIZE)
#undef GUEST_SIZE
#define GUEST_SIZE TEST_GUEST_SIZE
#undef GUEST_LAYOUT_END
#define GUEST_LAYOUT_END TEST_GUEST_SIZE
#endif
#if defined(TEST_GUEST_LAYOUT_END)
#undef GUEST_LAYOUT_END
#define GUEST_LAYOUT_END TEST_GUEST_LAYOUT_END
#endif
#include "runtime/x86.h"
#include "platform/os.h"
#include <stdio.h>
#include <stdlib.h>

static_assert(GUEST_HEAP_END < GUEST_STACK_TOP - GUEST_STACK_SIZE);
static_assert(GUEST_STACK_TOP < GUEST_RETURN_SENTINEL);
static_assert(GUEST_RETURN_SENTINEL < GUEST_TEB_BASE);
static_assert(GUEST_TEB_BASE < GUEST_SHIM_BASE);
static_assert(GUEST_SHIM_END == GUEST_LAYOUT_END);
static_assert(GUEST_LAYOUT_END <= GUEST_SIZE);

static void fault(const char *) {
    os_exit_immediately(201);
}

int main(int argc, char **argv) {
    uint8_t *arena = (uint8_t *)os_guest_arena_reserve(GUEST_SIZE);
    if (!arena)
        return 1;
    arena[0] = 0x12;
    arena[GUEST_SIZE - 1] = 0x34;
    if (arena[0] != 0x12 || arena[GUEST_SIZE - 1] != 0x34)
        return 2;
    if (GUEST_LAYOUT_END < GUEST_SIZE) {
        // A high module remains intact while every allocating region is used.
        arena[GUEST_LAYOUT_END] = 0x56;
        const uint32_t addresses[] = {GUEST_HEAP_BASE, GUEST_HEAP_END - 1, GUEST_STACK_TOP - 1,
                                      GUEST_TEB_BASE, GUEST_SHIM_END - 1};
        for (uint32_t address : addresses)
            arena[address] = 0x78;
        if (arena[GUEST_LAYOUT_END] != 0x56)
            return 8;
    }
    if (argc == 2) {
        if (!os_install_fault_handlers(fault))
            return 3;
        const unsigned test = (unsigned)strtoul(argv[1], nullptr, 10);
        const uint64_t offsets[] = {GUEST_SIZE, GUEST_SIZE - 1, UINT64_C(0xfffffffc),
                                    UINT64_C(0xffffffff)};
        if (test >= 4)
            return 4;
        uint64_t value = 0;
        for (unsigned byte = 0; byte < 8; ++byte)
            value |= uint64_t(*(volatile uint8_t *)(arena + offsets[test] + byte)) << (8 * byte);
        (void)value;
        return 5; // No signal: memory outside the guest was readable.
    }
    os_guest_arena_release(arena, GUEST_SIZE);
    char self[4096];
    if (os_exe_path(self, sizeof self) != 0)
        return 6;
    for (unsigned i = 0; i < 4; ++i) {
        char arg[8];
        snprintf(arg, sizeof arg, "%u", i);
        const char *child[] = {self, arg, nullptr};
        int64_t pid = 0;
        int status = 0;
        if (os_spawn(child, &pid) != 0 || os_wait(pid, &status) != 0 || status != 201) {
            fprintf(stderr, "guard probe %u: exit %d, expected access violation\n", i, status);
            return 7;
        }
    }
    printf("guest memory guards passed (%u MiB)\n", GUEST_SIZE >> 20);
    return 0;
}
