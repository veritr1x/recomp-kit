// Compile this translation unit with developer loaders forced off even when
// the surrounding build opted in. Ordinary switches remain available.
#undef RECOMP_DEV_LOADERS
#define RECOMP_DEV_LOADERS 0
#include "platform/os.h"
#include <cstring>

int main() {
    os_setenv("RECOMP_EXTRA_CODE", "hostile-library");
    os_setenv("RECOMP_VULKAN_LIBRARY", "hostile-driver");
    os_setenv("RECOMP_LOG", "1");
    return recomp_dev_env("EXTRA_CODE") || recomp_dev_env("VULKAN_LIBRARY") || !recomp_env("LOG") ||
           std::strcmp(recomp_env("LOG"), "1") != 0;
}
