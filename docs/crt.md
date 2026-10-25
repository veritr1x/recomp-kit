# VC8 CRT compatibility

The runtime serves `msvcr80.dll` and `msvcp80.dll` directly. No host CRT DLL is
loaded, and guest pointers, `va_list`, `FILE`, `tm`, `wchar_t`, and C++ objects
retain their 32-bit Windows layouts on every host. `runtime/crt_internal.h`
defines the checked boundary shared by the implementation groups.

`crt_core.cpp` implements bulk memory, allocation (including alignment/offset
variants), errno/doserrno, the MSVC random sequence, qsort and bsearch. Bulk
lengths are checked with 64-bit arithmetic before touching host memory.
Sorting invokes guest comparators without keeping an owning native allocation
across a callback. Errno, random state, tokenization state and time scratch
storage belong to the guest TEB; thread cleanup frees them while holding the
scheduler baton.

`crt_strings.cpp` serves narrow and UTF-16 string operations, secure copies,
tokenization and 32/64-bit integer conversion. `crt_math.cpp` implements math,
x87 `_CI` and SSE2 helpers, classification, conversions and FP control/status.
Control changes affect the virtual x87 control word and MXCSR, including their
independent rounding and denormal bits.

`crt_stdio.cpp` parses guest argument words itself. Numeric scalar formatting
uses a constructed, validated native format; no guest argument list reaches
native varargs. Width and precision are limited to 1 MiB, total formatted
output to 16 MiB. Legacy `_snprintf` truncation, null-buffer length queries,
signed VC8 indefinite/NaN spellings and secure null termination are covered.
Files use stable descriptor-map nodes and 32-byte guest FILE records. Text
mode translates CRLF; scans retain unused lookahead. All paths go through
the existing operation-specific resolver, so mutations require the writable
profile overlay. No original game asset becomes writable through CRT fopen.

`crt_startup.cpp` provides initialized data exports, argv, a guest environment
shared with Win32, initializer/exit tables, CRT threads, time, and C/en-US
locale helpers. It never copies the host process environment into the guest.
The command-line data pointers are the same allocations returned by Win32.
Wide argv and environment strings decode that seam's UTF-8 into UTF-16,
including surrogate pairs; wide environment updates convert back to UTF-8.
ExitThread/ExitProcess forwarding dispatches in place without adding a
callback ownership record that the nonlocal exit would strand.

`crt_cpp.cpp` implements VC8 basic_string for char and two-byte wchar_t:
allocator prefix at +0, 16-byte inline/pointer union at +4, size at +20 and
capacity at +24. Inline capacities are 15 and 7 respectively. Mutations keep
their source alive until the replacement is complete, including source views
inside the same string. Runtime-generated bad_alloc, length_error and
out_of_range use the C++ checkpoint dispatcher directly; no foreign callback
boundary is invented between a library operation and its caller. Sixteen
reserved throwing objects support nested exceptions without allocating the
object during an allocation failure. Exceeding that limit is a named fatal
error.

This is a compatibility subset, not every export in Microsoft's CRT. Iostream
members, `_setjmp3`, wide file scanning and unregistered APIs stop by import
name when required by an IAT. Optional GetProcAddress probes for unavailable
exports return zero. Locale support is C and en-US/1252; wide file output is
UTF-16LE and non-ASCII wide strftime patterns are refused. Additional locales,
CRT invalid-parameter callbacks and arbitrary C++ iterator/debug layouts are
not implemented. The private 347-import game list mentioned in issue #25 is
not present in this repository, so its exact coverage and game playability
have not been verified.

DLL `_initterm` and `_initterm_e` calls are also recognized by the translator:
an absolute IAT call or a pure IAT jump thunk names the initializer range.
The existing checks still require an aligned bounded data table containing
only nulls and executable entry addresses.
