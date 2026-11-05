// The web guest worker mounts a read-only HTTP install before boot_load.
#pragma once
#include <string>

bool web_stream_mount(const std::string &base_url, std::string *error);
