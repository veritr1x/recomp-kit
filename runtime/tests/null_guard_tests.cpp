// Exercise every scalar guest access with the debug check compiled in.
// Each armed access runs in its own process because the unhandled fault exits.
#undef RECOMP_NULL_CHECKS
#define RECOMP_NULL_CHECKS 1
#include "runtime/memory.h"
#include "platform/os.h"
#include <cstdio>
#include <cstdlib>

static void access(unsigned mode) {
    switch (mode) {
    case 0:
        (void)rd8(0x15);
        break;
    case 1:
        (void)rd16(0x15);
        break;
    case 2:
        (void)rd32(0x15);
        break;
    case 3:
        (void)rd64(0x15);
        break;
    case 4:
        (void)rdf32(0x15);
        break;
    case 5:
        (void)rdf64(0x15);
        break;
    case 6:
        wr8(0x15, 1);
        break;
    case 7:
        wr16(0x15, 1);
        break;
    case 8:
        wr32(0x15, 1);
        break;
    case 9:
        wr64(0x15, 1);
        break;
    case 10:
        wrf32(0x15, 1);
        break;
    case 11:
        wrf64(0x15, 1);
        break;
    case 12:
        wrbcd80(0x15, 1);
        break;
    case 13:
        wrf80(0x15, 1);
        break;
    case 14:
        (void)rdf80(0x15);
        break;
    }
}

static void fatal_access(const char *) {
    os_exit_immediately(134);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        os_install_fault_handlers(fatal_access);
        os_setenv("RECOMP_NULL_FAULTS", "1");
        mem_init();
        access((unsigned)std::strtoul(argv[1], nullptr, 10));
        return 1; // An armed low-page access must not return normally.
    }
    os_unsetenv("RECOMP_NULL_FAULTS");
    mem_init();
    for (unsigned i = 0; i < 15; ++i)
        access(i); // Disarmed debug checks retain the existing flat-page behavior.
    mem_shutdown();
    char self[4096];
    if (os_exe_path(self, sizeof self) != 0)
        return 2;
    for (unsigned i = 0; i < 15; ++i) {
        char mode[8];
        std::snprintf(mode, sizeof mode, "%u", i);
        const char *args[] = {self, mode, nullptr};
        int64_t pid = 0;
        int status = 0;
        if (os_spawn(args, &pid) != 0 || os_wait(pid, &status) != 0 || status != 134) {
            std::fprintf(stderr, "null access %u: exit %d, expected 134\n", i, status);
            return 3;
        }
    }
    return 0;
}
