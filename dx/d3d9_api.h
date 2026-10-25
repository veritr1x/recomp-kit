// Shared device/resource operations for the version 8 and 9 guest ABIs.
// Arguments are decoded by their frontend; these cores never rewrite a guest stack.
#pragma once
#include "com.h"

uint32_t d9_api_view(ComObj *object, ComIface version9_interface);
const ComMethod *d9_api_method(ComIface version9_interface, const char *name);
ComObj *d9_api_surface(ComObj *device, uint32_t width, uint32_t height, uint32_t format,
                       uint32_t sample_type, uint32_t usage, uint32_t pool);
void d9_api_stream(ComObj *device, uint32_t stream, ComObj *buffer, uint32_t offset,
                   uint32_t stride);
void d9_api_indices(ComObj *device, ComObj *buffer);
void d9_api_indexed_draw(ComObj *device, uint32_t primitive, int32_t base_vertex,
                         uint32_t first_index, uint32_t primitive_count);
