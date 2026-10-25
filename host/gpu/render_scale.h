// Small bookkeeping targets are not evidence for a game's rendering size.
#pragma once
#include <cstdint>

namespace d9gpu {
inline bool set_render_scale_base(uint32_t rows, uint32_t &base_rows) {
    if (rows < 64)
        return false;
    base_rows = rows;
    return true;
}
} // namespace d9gpu
