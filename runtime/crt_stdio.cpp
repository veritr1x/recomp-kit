// VC8 stdio: parse guest varargs explicitly, never pass a guest va_list to libc.
#include "crt_internal.h"
#include "win32.h"
#include "../platform/os.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>

namespace crt {
namespace {
constexpr uint32_t bound = 1u << 20, output_bound = 16u << 20;
using Units = std::u16string;
uint32_t unit(uint32_t p, uint32_t i, bool wide) {
    const uint64_t a = uint64_t(p) + uint64_t(i) * (wide ? 2 : 1);
    if (a > UINT32_MAX)
        fail("text offset wraps");
    range(uint32_t(a), wide ? 2 : 1);
    return wide ? rd16(uint32_t(a)) : rd8(uint32_t(a));
}
void store(uint32_t p, uint32_t i, uint32_t value, bool wide) {
    range(p, uint64_t(i + 1) * (wide ? 2 : 1));
    if (wide)
        wr16(p + i * 2, uint16_t(value));
    else
        wr8(p + i, uint8_t(value));
}
Units read_units(uint32_t p, bool wide, uint32_t limit = bound, bool limited = false) {
    Units result;
    for (uint32_t i = 0; i < limit; ++i) {
        auto ch = unit(p, i, wide);
        if (!ch)
            return result;
        result.push_back(char16_t(ch));
    }
    if (!limited)
        fail("unterminated format or text", p, limit);
    return result;
}
void append(Units &out, const Units &s) {
    if (uint64_t(out.size()) + s.size() > output_bound)
        fail("formatted output exceeds limit");
    out += s;
}
void repeat(Units &out, uint32_t n, char16_t ch) {
    if (uint64_t(out.size()) + n > output_bound)
        fail("formatted output exceeds limit");
    out.append(n, ch);
}
Units narrow(const std::string &s) {
    return Units(s.begin(), s.end());
}
uint32_t decimal(const Units &f, size_t &i) {
    uint32_t value = 0;
    while (i < f.size() && f[i] >= '0' && f[i] <= '9') {
        if (value > bound / 10)
            fail("format width or precision exceeds limit");
        value = value * 10 + f[i++] - '0';
        if (value > bound)
            fail("format width or precision exceeds limit");
    }
    return value;
}
enum Length { Normal, Short, Byte, Long, LongLong, Wide };
// Width and precision are capped before any native allocation or printf call.
Units format(uint32_t fmt, Args args, bool wide) {
    const Units f = read_units(fmt, wide);
    Units out;
    for (size_t i = 0; i < f.size();) {
        if (f[i] != '%') {
            repeat(out, 1, f[i++]);
            continue;
        }
        ++i;
        if (i < f.size() && f[i] == '%') {
            repeat(out, 1, '%');
            ++i;
            continue;
        }
        std::string flags;
        while (i < f.size() &&
               (f[i] == '-' || f[i] == '+' || f[i] == ' ' || f[i] == '#' || f[i] == '0')) {
            if (flags.find(char(f[i])) == std::string::npos)
                flags += char(f[i]);
            ++i;
        }
        uint32_t width = 0;
        int32_t precision = -1;
        if (i < f.size() && f[i] == '*') {
            ++i;
            int32_t w = int32_t(args.u32());
            if (w == INT32_MIN || uint32_t(w < 0 ? -w : w) > bound)
                fail("format width or precision exceeds limit");
            if (w < 0) {
                flags += '-';
                w = -w;
            }
            width = uint32_t(w);
        } else
            width = decimal(f, i);
        if (i < f.size() && f[i] == '.') {
            ++i;
            if (i < f.size() && f[i] == '*') {
                ++i;
                precision = int32_t(args.u32());
                if (precision < 0)
                    precision = -1;
            } else
                precision = int32_t(decimal(f, i));
            if (precision > int32_t(bound))
                fail("format width or precision exceeds limit");
        }
        Length len = Normal;
        if (i < f.size() && f[i] == 'h') {
            ++i;
            len = Short;
            if (i < f.size() && f[i] == 'h') {
                ++i;
                len = Byte;
            }
        } else if (i < f.size() && f[i] == 'l') {
            ++i;
            len = Long;
            if (i < f.size() && f[i] == 'l') {
                ++i;
                len = LongLong;
            }
        } else if (i < f.size() && f[i] == 'w') {
            ++i;
            len = Wide;
        } else if (i + 2 < f.size() && f[i] == 'I' && f[i + 1] == '6' && f[i + 2] == '4') {
            i += 3;
            len = LongLong;
        } else if (i + 2 < f.size() && f[i] == 'I' && f[i + 1] == '3' && f[i + 2] == '2') {
            i += 3;
        } else if (i < f.size() && (f[i] == 'I' || f[i] == 'z' || f[i] == 't' || f[i] == 'L'))
            ++i;
        if (i == f.size())
            fail("incomplete format conversion");
        const char conv = char(f[i++]);
        Units piece;
        bool padded = false;
        if (conv == 'n') {
            const uint32_t p = args.u32();
            range(p, len == LongLong ? 8 : len == Short ? 2 : len == Byte ? 1 : 4);
            if (len == LongLong)
                wr64(p, out.size());
            else if (len == Short)
                wr16(p, uint16_t(out.size()));
            else if (len == Byte)
                wr8(p, uint8_t(out.size()));
            else
                wr32(p, uint32_t(out.size()));
            continue;
        }
        if (conv == 's' || conv == 'S' || conv == 'c' || conv == 'C') {
            bool input_wide = (conv == 's' || conv == 'c') ? wide : !wide;
            if (len == Short)
                input_wide = false;
            if (len == Long || len == Wide)
                input_wide = true;
            const uint32_t value = args.u32();
            if (conv == 'c' || conv == 'C')
                piece.push_back(char16_t(input_wide ? value & 65535 : value & 255));
            else if (!value)
                piece = u"(null)";
            else
                piece = read_units(value, input_wide, precision < 0 ? bound : uint32_t(precision),
                                   precision >= 0);
            if (precision >= 0 && conv != 'c' && conv != 'C' && piece.size() > uint32_t(precision))
                piece.resize(precision);
        } else {
            std::string spec = "%" + flags;
            if (width)
                spec += std::to_string(width);
            if (precision >= 0)
                spec += '.' + std::to_string(precision);
            std::string bytes;
            int count = -1;
            auto render = [&](auto value) {
                const int needed = std::snprintf(nullptr, 0, spec.c_str(), value);
                if (needed < 0 || needed > int(output_bound))
                    fail("invalid numeric format");
                bytes.resize(size_t(needed) + 1);
                std::snprintf(bytes.data(), bytes.size(), spec.c_str(), value);
                bytes.resize(needed);
            };
            if (conv == 'd' || conv == 'i') {
                int64_t value = len == LongLong ? int64_t(args.u64()) : int32_t(args.u32());
                if (len == Short)
                    value = int16_t(value);
                if (len == Byte)
                    value = int8_t(value);
                spec += "ll";
                spec += conv;
                render((long long)value);
            } else if (conv == 'u' || conv == 'o' || conv == 'x' || conv == 'X') {
                uint64_t value = len == LongLong ? args.u64() : args.u32();
                if (len == Short)
                    value = uint16_t(value);
                if (len == Byte)
                    value = uint8_t(value);
                spec += "ll";
                spec += conv;
                render((unsigned long long)value);
            } else if (conv == 'p') {
                const uint32_t p = args.u32();
                char hex[9];
                std::snprintf(hex, sizeof hex, "%08X", p);
                bytes = hex;
            } else if (std::strchr("aAeEfFgG", conv)) {
                const double value = args.f64();
                if (std::isnan(value) || std::isinf(value)) {
                    const bool neg = std::signbit(value);
                    bytes = neg                                    ? "-"
                            : flags.find('+') != std::string::npos ? "+"
                            : flags.find(' ') != std::string::npos ? " "
                                                                   : "";
                    const uint32_t digits = precision < 0 ? 6 : uint32_t(precision);
                    std::string special = std::isinf(value) ? "1.#INF" : neg ? "1.#IND" : "1.#QNAN";
                    // VC8 pads/truncates the legacy exponent-free spelling to
                    // the requested number of digits following its decimal.
                    const size_t wanted = 2 + digits;
                    if (special.size() < wanted)
                        special.append(wanted - special.size(), '0');
                    else if (special.size() > wanted)
                        special.resize(wanted);
                    bytes += special;
                    piece = narrow(bytes);
                } else {
                    spec += conv;
                    render(value);
                }
            } else
                fail("unsupported format conversion", uint32_t(uint8_t(conv)));
            (void)count;
            if (piece.empty()) {
                piece = narrow(bytes);
                padded = conv != 'p';
            }
        }
        const uint32_t pad = width > piece.size() ? width - uint32_t(piece.size()) : 0;
        if (!padded && flags.find('-') == std::string::npos)
            repeat(out, pad, ' ');
        append(out, piece);
        if (!padded && flags.find('-') != std::string::npos)
            repeat(out, pad, ' ');
    }
    return out;
}
template <bool WideOut, bool V = false, int Kind = 0> void string_print(X86 *c) {
    const uint32_t dest = arg(c, 0), cap = Kind == 0 ? output_bound : arg(c, 1),
                   fmt = arg(c, Kind == 0 ? 1 : 2);
    Args args{V ? arg(c, Kind == 0 ? 2 : 3) : c->r[R_ESP] + 4 * (Kind == 0 ? 3 : 4)};
    const auto out = format(fmt, args, WideOut);
    if (!dest && cap == 0) {
        set_eax(c, uint32_t(out.size()));
        return;
    }
    const uint32_t copy = std::min<uint32_t>(uint32_t(out.size()), cap);
    range(dest, uint64_t(copy) * (WideOut ? 2 : 1));
    for (uint32_t i = 0; i < copy; ++i)
        store(dest, i, out[i], WideOut);
    if (out.size() < cap)
        store(dest, uint32_t(out.size()), 0, WideOut);
    // Legacy _snprintf writes no terminator when the result fills the buffer.
    set_eax(c, out.size() > cap ? UINT32_MAX : uint32_t(out.size()));
}
template <bool WideOut = false, bool V = false, bool Count = true> void secure_print(X86 *c) {
    const uint32_t dest = arg(c, 0), cap = arg(c, 1), count = Count ? arg(c, 2) : UINT32_MAX,
                   first = Count ? 3 : 2;
    if (!dest || !cap) {
        set_errno(c, EINVAL_);
        set_eax(c, UINT32_MAX);
        return;
    }
    const auto out =
        format(arg(c, first), Args{V ? arg(c, first + 1) : c->r[R_ESP] + 4 * (first + 2)}, WideOut);
    if (out.size() >= cap && (!Count || (count >= cap && count != UINT32_MAX))) {
        store(dest, 0, 0, WideOut);
        set_errno(c, ERANGE_);
        set_eax(c, UINT32_MAX);
        return;
    }
    const uint32_t n = std::min<uint32_t>(uint32_t(out.size()), std::min(cap - 1, count));
    range(dest, uint64_t(n + 1) * (WideOut ? 2 : 1));
    for (uint32_t i = 0; i < n; ++i)
        store(dest, i, out[i], WideOut);
    store(dest, n, 0, WideOut);
    set_eax(c, n < out.size() ? UINT32_MAX : n);
}
template <bool WideOut, bool V = false> void count_print(X86 *c) {
    auto out = format(arg(c, 0), Args{V ? arg(c, 1) : c->r[R_ESP] + 8}, WideOut);
    set_eax(c, uint32_t(out.size()));
}

struct File {
    int host = -1;
    bool owned = false, text = true, append = false, eof = false, error = false;
    int push = -1;
    uint32_t stream = 0;
    std::deque<uint8_t> scan_back;
};
// Map nodes are stable across opens from other cooperative guest threads.
std::map<uint32_t, File> files;
uint32_t next_fd = 3;
uint64_t generation = 0;
void reset() {
    if (generation == mem_generation())
        return;
    for (auto &[id, f] : files)
        if (f.owned)
            os_fd_close(f.host);
    files.clear();
    next_fd = 3;
    for (uint32_t i = 0; i < 3; ++i) {
        File f;
        f.host = int(i);
        files.emplace(i, f);
    }
    generation = mem_generation();
}
File *file(X86 *c, uint32_t id) {
    reset();
    auto it = files.find(id);
    if (it == files.end()) {
        set_errno(c, EBADF_);
        return nullptr;
    }
    return &it->second;
}
void iob_init(uint32_t p) {
    reset();
    for (uint32_t i = 0; i < 3; ++i) {
        files[i].stream = p + i * 32;
        wr32(p + i * 32 + 12, i ? 2 : 1);
        wr32(p + i * 32 + 16, i);
    }
}
File *stream(X86 *c, uint32_t p) {
    range(p, 32);
    File *f = file(c, rd32(p + 16));
    if (!f || f->stream != p) {
        set_errno(c, EBADF_);
        return nullptr;
    }
    return f;
}
uint32_t stream_address(uint32_t id) {
    auto &f = files.at(id);
    if (!f.stream) {
        f.stream = heap_alloc(32, true);
        if (!f.stream)
            fail("cannot allocate FILE");
        wr32(f.stream + 16, id);
        wr32(f.stream + 12, 0x80);
    }
    return f.stream;
}
uint32_t host_error(X86 *c) {
    uint32_t e = errno == ENOENT   ? ENOENT_
                 : errno == EBADF  ? EBADF_
                 : errno == EACCES ? EACCES_
                 : errno == ENOMEM ? ENOMEM_
                                   : EINVAL_;
    set_errno(c, e);
    return UINT32_MAX;
}
std::string path(uint32_t p, bool wide, int op) {
    if (!wide)
        return win32_host_path_op(text(p), op);
    const auto w = wtext(p);
    std::string s;
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t ch = w[i];
        if (ch >= 0xd800 && ch <= 0xdbff && i + 1 < w.size() && w[i + 1] >= 0xdc00 &&
            w[i + 1] <= 0xdfff)
            ch = 0x10000 + ((ch - 0xd800) << 10) + (w[++i] - 0xdc00);
        if (ch < 128)
            s += char(ch);
        else if (ch < 2048) {
            s += char(0xc0 | (ch >> 6));
            s += char(0x80 | (ch & 63));
        } else if (ch < 65536) {
            s += char(0xe0 | (ch >> 12));
            s += char(0x80 | ((ch >> 6) & 63));
            s += char(0x80 | (ch & 63));
        } else {
            s += char(0xf0 | (ch >> 18));
            s += char(0x80 | ((ch >> 12) & 63));
            s += char(0x80 | ((ch >> 6) & 63));
            s += char(0x80 | (ch & 63));
        }
    }
    return win32_host_path_op(s, op);
}
uint32_t open_file(X86 *c, uint32_t p, bool wide, int flags, bool text_mode, bool append_mode) {
    reset();
    const bool writing = (flags & 3) != OS_O_RDONLY || (flags & OS_O_CREAT);
    const auto resolved = path(p, wide, writing ? WIN32_FILE_WRITE : WIN32_FILE_READ);
    if (resolved.empty()) {
        set_errno(c, EACCES_);
        return UINT32_MAX;
    }
    const int fd = os_fd_open(resolved.c_str(), flags);
    if (fd < 0)
        return host_error(c);
    if (next_fd == UINT32_MAX) {
        os_fd_close(fd);
        set_errno(c, 24);
        return UINT32_MAX;
    }
    File f;
    f.host = fd;
    f.owned = true;
    f.text = text_mode;
    f.append = append_mode;
    const uint32_t id = next_fd++;
    files.emplace(id, f);
    return id;
}
template <bool WidePath = false> void fd_open(X86 *c) {
    const uint32_t flags = arg(c, 1);
    int native = int(flags & 3);
    if (flags & 0x100)
        native |= OS_O_CREAT;
    if (flags & 0x200)
        native |= OS_O_TRUNC;
    if (flags & 0x400)
        native |= OS_O_EXCL;
    set_eax(c, open_file(c, arg(c, 0), WidePath, native, !(flags & 0x8000), flags & 8));
}
template <bool WidePath = false> void fopen_(X86 *c) {
    const auto mode = read_units(arg(c, 1), WidePath, 32);
    if (mode.empty() || (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a')) {
        set_errno(c, EINVAL_);
        set_eax(c, 0);
        return;
    }
    const bool plus = mode.find('+') != Units::npos;
    int flags = plus ? OS_O_RDWR : mode[0] == 'r' ? OS_O_RDONLY : OS_O_WRONLY;
    if (mode[0] != 'r')
        flags |= OS_O_CREAT;
    if (mode[0] == 'w')
        flags |= OS_O_TRUNC;
    const uint32_t id =
        open_file(c, arg(c, 0), WidePath, flags, mode.find('b') == Units::npos, mode[0] == 'a');
    set_eax(c, id == UINT32_MAX ? 0 : stream_address(id));
}
int get(File &f) {
    if (!f.scan_back.empty()) {
        const int ch = f.scan_back.front();
        f.scan_back.pop_front();
        f.eof = false;
        return ch;
    }
    if (f.push >= 0) {
        int ch = f.push;
        f.push = -1;
        f.eof = false;
        return ch;
    }
    uint8_t ch;
    const auto n = os_fd_read(f.host, &ch, 1);
    if (n <= 0) {
        f.eof = n == 0;
        f.error = n < 0;
        return -1;
    }
    if (f.text && ch == 26) {
        f.eof = true;
        return -1;
    }
    if (f.text && ch == 13) {
        uint8_t next;
        const auto more = os_fd_read(f.host, &next, 1);
        if (more == 1) {
            if (next == 10)
                return 10;
            f.push = next;
        } else if (more < 0)
            f.error = true;
    }
    return ch;
}
uint64_t write(File &f, const uint8_t *data, uint32_t count) {
    if (f.append && os_fd_seek(f.host, 0, OS_SEEK_END) < 0) {
        f.error = true;
        return 0;
    }
    uint32_t done = 0;
    while (done < count) {
        if (f.text && data[done] == 10) {
            const uint8_t cr = 13;
            if (os_fd_write(f.host, &cr, 1) != 1) {
                f.error = true;
                break;
            }
        }
        uint32_t chunk = count - done;
        if (f.text) {
            chunk = 1;
            if (data[done] != 10)
                while (done + chunk < count && data[done + chunk] != 10)
                    ++chunk;
        }
        const auto n = os_fd_write(f.host, data + done, chunk);
        if (n <= 0) {
            f.error = true;
            break;
        }
        done += uint32_t(n);
    }
    return done;
}
template <bool Reading, bool Stream> void transfer(X86 *c) {
    const uint32_t p = arg(c, Stream ? 0 : 1);
    const uint64_t count = Stream ? uint64_t(arg(c, 1)) * arg(c, 2) : arg(c, 2);
    range(p, count);
    File *f = Stream ? stream(c, arg(c, 3)) : file(c, arg(c, 0));
    if (!f) {
        set_eax(c, Stream ? 0 : UINT32_MAX);
        return;
    }
    uint64_t done = 0;
    if (Reading) {
        while (done < count) {
            int ch = get(*f);
            if (ch < 0)
                break;
            wr8(p + uint32_t(done++), uint8_t(ch));
        }
    } else
        done = write(*f, gm_ptr(p), uint32_t(count));
    if (f->error)
        set_errno(c, EINVAL_);
    set_eax(c, Stream              ? (arg(c, 1) ? uint32_t(done / arg(c, 1)) : 0)
               : f->error && !done ? UINT32_MAX
                                   : uint32_t(done));
}
template <bool Stream> void close_(X86 *c) {
    File *f = Stream ? stream(c, arg(c, 0)) : file(c, arg(c, 0));
    if (!f) {
        set_eax(c, UINT32_MAX);
        return;
    }
    const uint32_t id = Stream ? rd32(arg(c, 0) + 16) : arg(c, 0);
    int result = f->owned ? os_fd_close(f->host) : 0;
    if (f->stream) {
        wr32(f->stream + 12, 0);
        if (id >= 3)
            heap_free(f->stream);
    }
    files.erase(id);
    set_eax(c, result ? host_error(c) : 0);
}
template <bool Stream, bool Large = false, bool Tell = false> void seek_(X86 *c) {
    File *f = Stream ? stream(c, arg(c, 0)) : file(c, arg(c, 0));
    if (!f) {
        set_eax(c, UINT32_MAX);
        c->r[R_EDX] = UINT32_MAX;
        return;
    }
    int64_t offset = Tell    ? 0
                     : Large ? int64_t(uint64_t(arg(c, 1)) | uint64_t(arg(c, 2)) << 32)
                             : int32_t(arg(c, 1));
    uint32_t whence = Tell ? 1 : arg(c, Large ? 3 : 2);
    if (whence > 2) {
        set_errno(c, EINVAL_);
        set_eax(c, UINT32_MAX);
        return;
    }
    if (whence == 1)
        offset -= int64_t(f->scan_back.size()) + (f->push >= 0 ? 1 : 0);
    const int64_t result = os_fd_seek(f->host, offset, int(whence));
    if (result < 0) {
        set_eax(c, host_error(c));
        c->r[R_EDX] = UINT32_MAX;
        return;
    }
    if (!Tell) {
        f->push = -1;
        f->scan_back.clear();
        f->eof = false;
    }
    set_eax(c, Stream && !Tell ? 0 : uint32_t(result));
    if (Large)
        c->r[R_EDX] = uint32_t(uint64_t(result) >> 32);
}
void fileno_(X86 *c) {
    auto f = stream(c, arg(c, 0));
    set_eax(c, f ? rd32(arg(c, 0) + 16) : UINT32_MAX);
}
void eof_(X86 *c) {
    auto f = stream(c, arg(c, 0));
    set_eax(c, f && f->eof ? 0x10 : 0);
}
void error_(X86 *c) {
    auto f = stream(c, arg(c, 0));
    set_eax(c, f && f->error ? 0x20 : 0);
}
void clear_(X86 *c) {
    auto f = stream(c, arg(c, 0));
    if (f) {
        f->error = false;
        f->eof = false;
    }
}
void flush_(X86 *c) {
    if (!arg(c, 0)) {
        set_eax(c, 0);
        return;
    }
    auto f = stream(c, arg(c, 0));
    set_eax(c, f ? 0 : UINT32_MAX);
}
void buffer_(X86 *c) {
    set_eax(c, stream(c, arg(c, 0)) ? 0 : UINT32_MAX);
}
void getc_(X86 *c) {
    auto f = stream(c, arg(c, 0));
    set_eax(c, f ? uint32_t(get(*f)) : UINT32_MAX);
}
void putc_(X86 *c) {
    auto f = stream(c, arg(c, 1));
    const uint8_t ch = uint8_t(arg(c, 0));
    set_eax(c, f && write(*f, &ch, 1) == 1 ? ch : UINT32_MAX);
}
void ungetc_(X86 *c) {
    auto f = stream(c, arg(c, 1));
    if (!f || f->push >= 0 || arg(c, 0) == UINT32_MAX) {
        set_eax(c, UINT32_MAX);
        return;
    }
    f->push = int(arg(c, 0) & 255);
    f->eof = false;
    set_eax(c, uint32_t(f->push));
}
void fgets_(X86 *c) {
    auto f = stream(c, arg(c, 2));
    const int32_t cap = int32_t(arg(c, 1));
    if (!f || cap <= 0) {
        set_eax(c, 0);
        return;
    }
    range(arg(c, 0), uint32_t(cap));
    uint32_t n = 0;
    while (n + 1 < uint32_t(cap)) {
        int ch = get(*f);
        if (ch < 0)
            break;
        wr8(arg(c, 0) + n++, uint8_t(ch));
        if (ch == 10)
            break;
    }
    wr8(arg(c, 0) + n, 0);
    set_eax(c, n || cap == 1 ? arg(c, 0) : 0);
}
void fputs_(X86 *c) {
    const uint32_t n = string_length(arg(c, 0), 1);
    auto f = stream(c, arg(c, 1));
    set_eax(c, f && write(*f, gm_ptr(arg(c, 0)), n) == n ? 0 : UINT32_MAX);
}
template <bool WidePath = false, bool Rename = false> void remove_(X86 *c) {
    const auto from = path(arg(c, 0), WidePath, Rename ? WIN32_FILE_RENAME_SRC : WIN32_FILE_DELETE);
    if (from.empty()) {
        set_errno(c, EACCES_);
        set_eax(c, UINT32_MAX);
        return;
    }
    int result;
    if (Rename) {
        const auto to = path(arg(c, 1), WidePath, WIN32_FILE_RENAME_DST);
        if (to.empty()) {
            set_errno(c, EACCES_);
            set_eax(c, UINT32_MAX);
            return;
        }
        result = os_rename(from.c_str(), to.c_str());
    } else
        result = os_unlink(from.c_str());
    set_eax(c, result ? host_error(c) : 0);
}
template <bool WideOut = false, bool V = false, bool Stream = false> void file_print(X86 *c) {
    File *f = Stream ? stream(c, arg(c, 0)) : file(c, 1);
    if (!f) {
        set_eax(c, UINT32_MAX);
        return;
    }
    const uint32_t first = Stream ? 1 : 0;
    const auto out =
        format(arg(c, first), Args{V ? arg(c, first + 1) : c->r[R_ESP] + 4 * (first + 2)}, WideOut);
    std::string bytes;
    for (auto ch : out) {
        bytes += char(ch & 255);
        if (WideOut)
            bytes += char(ch >> 8);
    }
    set_eax(c, write(*f, (const uint8_t *)bytes.data(), uint32_t(bytes.size())) == bytes.size()
                   ? uint32_t(out.size())
                   : UINT32_MAX);
}
void iob_(X86 *c) {
    set_eax(c, imports_alloc_data("msvcr80.dll", "_iob"));
}
void setmode_(X86 *c) {
    auto f = file(c, arg(c, 0));
    if (!f) {
        set_eax(c, UINT32_MAX);
        return;
    }
    const uint32_t mode = arg(c, 1);
    if (mode != 0x4000 && mode != 0x8000) {
        set_errno(c, EINVAL_);
        set_eax(c, UINT32_MAX);
        return;
    }
    const uint32_t old = f->text ? 0x4000 : 0x8000;
    f->text = mode == 0x4000;
    set_eax(c, old);
}
bool space(uint32_t ch) {
    return ch == ' ' || (ch >= 9 && ch <= 13);
}
// Scanning consumes only the matched prefix. A stream lookahead is restored
// through its ungetc slot, so a following fscanf/fgetc sees the same byte.
struct Input {
    File *file = nullptr;
    uint32_t address = 0, offset = 0;
    bool wide = false;
    int pending = -2;
    std::string rollback;
    int peek() {
        if (pending == -2)
            pending = file ? get(*file) : int(unit(address, offset, wide));
        return pending;
    }
    int take() {
        const int ch = peek();
        pending = -2;
        if (ch >= 0 && (file || ch))
            ++offset;
        return ch;
    }
    bool ended() {
        return peek() < 0 || (!file && peek() == 0);
    }
    void finish() {
        if (file) {
            if (pending >= 0)
                file->scan_back.push_front(uint8_t(pending));
            for (auto it = rollback.rbegin(); it != rollback.rend(); ++it)
                file->scan_back.push_front(uint8_t(*it));
            if (pending >= 0 || !rollback.empty())
                file->eof = false;
        }
    }
};
uint32_t scan(Input &in, uint32_t fmt, Args args, bool wide) {
    const auto f = read_units(fmt, wide);
    uint32_t assigned = 0;
    for (size_t i = 0; i < f.size();) {
        if (space(f[i])) {
            while (i < f.size() && space(f[i]))
                ++i;
            while (!in.ended() && space(in.peek()))
                in.take();
            continue;
        }
        if (f[i] != '%') {
            if (in.ended() || in.peek() != f[i])
                break;
            in.take();
            ++i;
            continue;
        }
        ++i;
        if (i < f.size() && f[i] == '%') {
            if (in.ended() || in.peek() != '%')
                break;
            in.take();
            ++i;
            continue;
        }
        bool suppress = i < f.size() && f[i] == '*';
        if (suppress)
            ++i;
        uint32_t width = decimal(f, i);
        if (!width)
            width = bound;
        Length len = Normal;
        if (i < f.size() && f[i] == 'h') {
            ++i;
            len = Short;
            if (i < f.size() && f[i] == 'h') {
                ++i;
                len = Byte;
            }
        } else if (i < f.size() && f[i] == 'l') {
            ++i;
            len = Long;
            if (i < f.size() && f[i] == 'l') {
                ++i;
                len = LongLong;
            }
        } else if (i + 2 < f.size() && f.substr(i, 3) == u"I64") {
            i += 3;
            len = LongLong;
        } else if (i < f.size() && f[i] == 'w') {
            ++i;
            len = Wide;
        }
        if (i >= f.size())
            fail("incomplete scan conversion");
        const char conv = char(f[i++]);
        const uint32_t dest = suppress ? 0 : args.u32();
        if (conv == 'n') {
            if (!suppress) {
                range(dest, len == LongLong ? 8 : len == Short ? 2 : len == Byte ? 1 : 4);
                if (len == LongLong)
                    wr64(dest, in.offset);
                else if (len == Short)
                    wr16(dest, uint16_t(in.offset));
                else if (len == Byte)
                    wr8(dest, uint8_t(in.offset));
                else
                    wr32(dest, in.offset);
            }
            continue;
        }
        if (conv != 'c' && conv != 'C' && conv != '[')
            while (!in.ended() && space(in.peek()))
                in.take();
        if (in.ended()) {
            if (!assigned)
                assigned = UINT32_MAX;
            break;
        }
        if (conv == 's' || conv == 'S' || conv == 'c' || conv == 'C' || conv == '[') {
            bool target_wide = (conv == 'S' || conv == 'C') ? !wide : wide;
            if (len == Short)
                target_wide = false;
            if (len == Long || len == Wide)
                target_wide = true;
            const bool character = conv == 'c' || conv == 'C';
            if (character && width == bound)
                width = 1;
            std::array<bool, 65536> accepted{};
            bool invert = false;
            if (conv == '[') {
                if (i < f.size() && f[i] == '^') {
                    invert = true;
                    ++i;
                }
                bool first = true;
                uint32_t previous = 0;
                while (i < f.size() && (f[i] != ']' || first)) {
                    const uint32_t ch = f[i++];
                    if (ch == '-' && !first && i < f.size() && f[i] != ']') {
                        const uint32_t end = f[i++];
                        if (previous <= end)
                            for (uint32_t k = previous; k <= end; ++k)
                                accepted[k] = true;
                        previous = end;
                    } else {
                        accepted[ch] = true;
                        previous = ch;
                    }
                    first = false;
                }
                if (i == f.size())
                    fail("unterminated scan set");
                ++i;
            }
            uint32_t n = 0;
            while (n < width && !in.ended()) {
                const uint32_t ch = uint32_t(in.peek());
                if (!character && (conv == '[' ? (accepted[ch & 65535] == invert) : space(ch)))
                    break;
                if (!suppress)
                    store(dest, n, ch, target_wide);
                in.take();
                ++n;
            }
            if (!n)
                break;
            if (!suppress) {
                if (!character)
                    store(dest, n, 0, target_wide);
                ++assigned;
            }
            continue;
        }
        const bool floating = std::strchr("aAeEfFgG", conv) != nullptr;
        const bool integer = std::strchr("diuoxXp", conv) != nullptr;
        if (!floating && !integer)
            fail("unsupported scan conversion", uint8_t(conv));
        std::string token;
        int base = conv == 'o'                                 ? 8
                   : conv == 'x' || conv == 'X' || conv == 'p' ? 16
                   : conv == 'i'                               ? 0
                                                               : 10;
        // Build a bounded candidate, then leave a nonnumeric delimiter unread.
        while (token.size() < width && !in.ended()) {
            const int ch = in.peek();
            bool allowed = floating ? ((ch >= '0' && ch <= '9') || ch == '.' || ch == '#' ||
                                       (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                       ch == '+' || ch == '-')
                                    : ((ch >= '0' && ch <= '9') ||
                                       ((base == 16 || base == 0) &&
                                        ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F') ||
                                         ch == 'x' || ch == 'X')) ||
                                       ((ch == '+' || ch == '-') && token.empty()));
            if (!allowed)
                break;
            token += char(in.take());
        }
        if (token.empty())
            break;
        char *end = nullptr;
        if (floating) {
            double value = std::strtod(token.c_str(), &end);
            const size_t at = token[0] == '-' || token[0] == '+' ? 1 : 0;
            if (token.compare(at, 6, "1.#IND") == 0 || token.compare(at, 7, "1.#QNAN") == 0 ||
                token.compare(at, 6, "1.#INF") == 0) {
                const bool inf = token.compare(at, 6, "1.#INF") == 0;
                uint64_t bits = (inf ? 0x7ff0000000000000ull : 0x7ff8000000000000ull) |
                                (token[0] == '-' ? 0x8000000000000000ull : 0);
                std::memcpy(&value, &bits, 8);
                size_t n = at + (token.compare(at, 7, "1.#QNAN") == 0 ? 7 : 6);
                while (n < token.size() && token[n] == '0')
                    ++n;
                end = token.data() + n;
            }
            if (end == token.c_str())
                break;
            if (!suppress) {
                range(dest, len == Long ? 8 : 4);
                if (len == Long) {
                    uint64_t bits;
                    std::memcpy(&bits, &value, 8);
                    wr64(dest, bits);
                } else {
                    const float small = float(value);
                    uint32_t bits;
                    std::memcpy(&bits, &small, 4);
                    wr32(dest, bits);
                }
                ++assigned;
            }
        } else {
            const uint64_t value = conv == 'd' || conv == 'i'
                                       ? uint64_t(std::strtoll(token.c_str(), &end, base))
                                       : std::strtoull(token.c_str(), &end, base);
            if (end == token.c_str())
                break;
            if (!suppress) {
                range(dest, len == LongLong ? 8 : len == Short ? 2 : len == Byte ? 1 : 4);
                if (len == LongLong)
                    wr64(dest, value);
                else if (len == Short)
                    wr16(dest, uint16_t(value));
                else if (len == Byte)
                    wr8(dest, uint8_t(value));
                else
                    wr32(dest, uint32_t(value));
                ++assigned;
            }
        }
        // strto* may stop before the candidate ends (e.g. %d on "12x").
        // Keep all unused lookahead for the next conversion/call. No native
        // FILE buffering or host descriptor address leaks into guest storage.
        const size_t unused = token.c_str() + token.size() - end;
        if (unused) {
            in.offset -= uint32_t(unused);
            if (in.file) {
                in.rollback.assign(end, unused);
                in.finish();
                in.pending = -2;
                in.rollback.clear();
            } else
                in.pending = -2;
        }
    }
    in.finish();
    return assigned;
}
template <bool WideInput = false, bool Stream = false, bool V = false> void scan_(X86 *c) {
    Input in;
    in.wide = WideInput;
    in.address = arg(c, 0);
    if (Stream) {
        in.file = stream(c, arg(c, 0));
        if (!in.file) {
            set_eax(c, UINT32_MAX);
            return;
        }
        if (WideInput)
            fail("wide file scanning is not implemented");
    }
    set_eax(c, scan(in, arg(c, 1), Args{V ? arg(c, 2) : c->r[R_ESP] + 12}, WideInput));
}
} // namespace
void register_stdio() {
#define E(name, ...)                                                                               \
    {                                                                                              \
        "msvcr80.dll", name, ARGC_CDECL, __VA_ARGS__                                               \
    }
    const ImportShim entries[] = {E("sprintf_s", secure_print<false, false, false>),
                                  E("vsprintf_s", secure_print<false, true, false>),
                                  E("swprintf_s", secure_print<true, false, false>),
                                  E("vswprintf_s", secure_print<true, true, false>),
                                  E("_snprintf_s", secure_print<>),
                                  E("_vsnprintf_s", secure_print<false, true>),
                                  E("_snwprintf_s", secure_print<true>),
                                  E("_vsnwprintf_s", secure_print<true, true>),
                                  E("sprintf", string_print<false>),
                                  E("vsprintf", string_print<false, true>),
                                  E("_snprintf", string_print<false, false, 1>),
                                  E("_vsnprintf", string_print<false, true, 1>),
                                  E("swprintf", string_print<true>),
                                  E("vswprintf", string_print<true, true>),
                                  E("_snwprintf", string_print<true, false, 1>),
                                  E("_vsnwprintf", string_print<true, true, 1>),
                                  E("_scprintf", count_print<false>),
                                  E("_vscprintf", count_print<false, true>),
                                  E("_scwprintf", count_print<true>),
                                  E("_vscwprintf", count_print<true, true>),
                                  E("printf", file_print<>),
                                  E("vprintf", file_print<false, true>),
                                  E("fprintf", file_print<false, false, true>),
                                  E("vfprintf", file_print<false, true, true>),
                                  E("wprintf", file_print<true>),
                                  E("vwprintf", file_print<true, true>),
                                  E("fwprintf", file_print<true, false, true>),
                                  E("vfwprintf", file_print<true, true, true>),
                                  E("_open", fd_open<>),
                                  E("_wopen", fd_open<true>),
                                  E("fopen", fopen_<>),
                                  E("_wfopen", fopen_<true>),
                                  E("_close", close_<false>),
                                  E("fclose", close_<true>),
                                  E("_read", transfer<true, false>),
                                  E("_write", transfer<false, false>),
                                  E("fread", transfer<true, true>),
                                  E("fwrite", transfer<false, true>),
                                  E("_lseek", seek_<false>),
                                  E("_lseeki64", seek_<false, true>),
                                  E("_tell", seek_<false, false, true>),
                                  E("_telli64", seek_<false, true, true>),
                                  E("fseek", seek_<true>),
                                  E("_fseeki64", seek_<true, true>),
                                  E("ftell", seek_<true, false, true>),
                                  E("_ftelli64", seek_<true, true, true>),
                                  E("_fileno", fileno_),
                                  E("feof", eof_),
                                  E("ferror", error_),
                                  E("clearerr", clear_),
                                  E("fflush", flush_),
                                  E("setvbuf", buffer_),
                                  E("fgetc", getc_),
                                  E("getc", getc_),
                                  E("fputc", putc_),
                                  E("putc", putc_),
                                  E("ungetc", ungetc_),
                                  E("fgets", fgets_),
                                  E("fputs", fputs_),
                                  E("remove", remove_<>),
                                  E("_unlink", remove_<>),
                                  E("_wremove", remove_<true>),
                                  E("_wunlink", remove_<true>),
                                  E("rename", remove_<false, true>),
                                  E("_wrename", remove_<true, true>),
                                  E("__iob_func", iob_),
                                  E("_setmode", setmode_),
                                  E("sscanf", scan_<>),
                                  E("swscanf", scan_<true>),
                                  E("fscanf", scan_<false, true>),
                                  E("vsscanf", scan_<false, false, true>)};
#undef E
    imports_register(entries, sizeof entries / sizeof entries[0]);
    imports_register_data_init("msvcr80.dll", "_iob", 96, iob_init);
}
} // namespace crt
