#include "d3d9_warm.h"
#include "../../dx/host_d9.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace d9warm {

Pool::Pool(unsigned threads) {
    for (unsigned i = 0; i < threads; ++i)
        if (OsThread *thread = os_thread_create(worker, this, 0))
            workers_.push_back(thread);
}
Pool::~Pool() {
    stop();
}
bool Pool::submit(std::function<void()> job) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || workers_.empty())
            return false;
        jobs_.push_back(std::move(job));
    }
    ready_.notify_one();
    return true;
}
void Pool::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    ready_.notify_all();
    for (OsThread *thread : workers_)
        os_thread_join(thread);
    workers_.clear();
}
size_t Pool::pending() const {
    std::lock_guard lock(mutex_);
    return jobs_.size();
}
void *Pool::worker(void *opaque) {
    auto &self = *static_cast<Pool *>(opaque);
    os_thread_prefer_user_initiated();
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(self.mutex_);
            self.ready_.wait(lock, [&] { return self.stopping_ || !self.jobs_.empty(); });
            if (self.stopping_)
                return nullptr;
            job = std::move(self.jobs_.front());
            self.jobs_.pop_front();
        }
        job();
    }
}

static std::string hex_bytes(const std::vector<uint8_t> &bytes) {
    static const char hex[] = "0123456789abcdef";
    std::string value;
    for (uint8_t byte : bytes) {
        value += hex[byte >> 4];
        value += hex[byte & 15];
    }
    return value;
}
std::string ShaderHint::line() const {
    std::ostringstream text;
    text << "S1 " << std::hex << code << ' ' << unsigned(pixel) << ' ' << variant;
    return text.str();
}
PipelineHint PipelineHint::canonical() const {
    PipelineHint copy = *this;
    copy.states[0] = copy.states[0] != 0;
    copy.states[4] = copy.states[4] != 0;
    if (!copy.states[0])
        std::fill(copy.states.begin() + 1, copy.states.begin() + 8, 0);
    else if (!copy.states[4])
        std::fill(copy.states.begin() + 5, copy.states.begin() + 8, 0);
    for (unsigned i = 8; i < 12; ++i)
        copy.states[i] &= 15;
    for (uint32_t &frequency : copy.frequencies)
        frequency = (frequency & HOST_D9_STREAM_INSTANCE) | host_d9_stream_rate(frequency);
    return copy;
}
std::string PipelineHint::line() const {
    const PipelineHint hint = canonical();
    std::ostringstream text;
    text << "P1 " << std::hex << hint.vertex << ' ' << hint.pixel << ' ' << hint.variant << ' '
         << hint.samples << ' ' << hint.depth;
    for (auto value : hint.colors)
        text << ' ' << value;
    for (auto value : hint.strides)
        text << ' ' << value;
    for (auto value : hint.frequencies)
        text << ' ' << value;
    for (auto value : hint.states)
        text << ' ' << value;
    text << ' ' << hex_bytes(hint.declaration);
    return text.str();
}
static bool finished(std::istringstream &text) {
    std::string extra;
    return !(text >> extra) && text.eof();
}
bool parse_shader_hint(const std::string &line, ShaderHint *out) {
    if (!out || line.size() > 4096 || line.find('-') != std::string::npos)
        return false;
    std::istringstream text(line);
    std::string tag;
    ShaderHint hint;
    unsigned pixel = 0;
    if (!(text >> tag >> std::hex >> hint.code >> pixel >> hint.variant) || tag != "S1" ||
        !hint.code || pixel > 1 || (!pixel && hint.variant) || !finished(text))
        return false;
    hint.pixel = pixel != 0;
    *out = hint;
    return true;
}
bool parse_pipeline_hint(const std::string &line, PipelineHint *out) {
    if (!out || line.size() > 4096 || line.find('-') != std::string::npos)
        return false;
    std::istringstream text(line);
    std::string tag, bytes;
    PipelineHint hint;
    if (!(text >> tag >> std::hex >> hint.vertex >> hint.pixel >> hint.variant >> hint.samples >>
          hint.depth) ||
        tag != "P1")
        return false;
    for (auto &value : hint.colors)
        if (!(text >> value) || value > 8192)
            return false;
    for (auto &value : hint.strides)
        if (!(text >> value) || value > 65536)
            return false;
    for (auto &value : hint.frequencies)
        if (!(text >> value) || !(value & HOST_D9_STREAM_DIVIDER) ||
            (value & HOST_D9_STREAM_INDEXED))
            return false;
    for (auto &value : hint.states)
        if (!(text >> value))
            return false;
    if (!(text >> bytes) || !finished(text) || !hint.vertex || !hint.pixel ||
        hint.vertex == hint.pixel || hint.depth > 8192 ||
        (hint.samples != 1 && hint.samples != 2 && hint.samples != 4 && hint.samples != 8) ||
        bytes.size() < 16 || bytes.size() > 1024 || bytes.size() % 16)
        return false;
    auto digit = [](char c) -> int {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    };
    for (size_t i = 0; i < bytes.size(); i += 2) {
        const int high = digit(bytes[i]), low = digit(bytes[i + 1]);
        if (high < 0 || low < 0)
            return false;
        hint.declaration.push_back(uint8_t(high * 16 + low));
    }
    if (hint.declaration[hint.declaration.size() - 8] != 0xff)
        return false;
    if (hint.states[0] > 1 || hint.states[4] > 1)
        return false;
    for (unsigned i : {1u, 2u, 5u, 6u})
        if (hint.states[i] > 17)
            return false;
    for (unsigned i : {3u, 7u})
        if (hint.states[i] > 5)
            return false;
    for (unsigned i = 8; i < 12; ++i)
        if (hint.states[i] > 15)
            return false;
    *out = hint.canonical();
    return true;
}

Hints::Hints(std::string profile) : profile_(std::move(profile)) {}
void Hints::load() {
    if (profile_.empty())
        return;
    std::ifstream file(profile_ + "/d3d9-warm.txt", std::ios::binary);
    std::string line;
    size_t total = 0;
    std::lock_guard lock(mutex_);
    bool oversized = false;
    char ch;
    while (file.get(ch)) {
        if (++total > 64u * 1024u * 1024u) {
            damaged_ = true;
            break;
        }
        if (ch != '\n') {
            if (line.size() < 4096)
                line += ch;
            else
                oversized = true;
            continue;
        }
        if (oversized) {
            damaged_ = true;
            line.clear();
            oversized = false;
            continue;
        }
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#') {
            line.clear();
            continue;
        }
        ShaderHint shader;
        PipelineHint pipeline;
        if (parse_shader_hint(line, &shader))
            lines_.insert(shader.line());
        else if (parse_pipeline_hint(line, &pipeline))
            lines_.insert(pipeline.line());
        else
            damaged_ = true;
        line.clear();
    }
    if (!line.empty() || oversized)
        damaged_ = true; // a torn final line is never trusted
    dirty_ |= damaged_;
    if (dirty_)
        ++revision_;
}
bool Hints::remember_line(std::string line) {
    std::lock_guard lock(mutex_);
    if (!lines_.insert(std::move(line)).second)
        return false;
    dirty_ = true;
    ++revision_;
    return true;
}
bool Hints::remember(const ShaderHint &hint) {
    return remember_line(hint.line());
}
bool Hints::remember(const PipelineHint &hint) {
    return remember_line(hint.line());
}
bool Hints::damaged() const {
    std::lock_guard lock(mutex_);
    return damaged_;
}
std::vector<ShaderHint> Hints::shaders() const {
    std::lock_guard lock(mutex_);
    std::vector<ShaderHint> result;
    for (const auto &line : lines_) {
        ShaderHint hint;
        if (parse_shader_hint(line, &hint))
            result.push_back(hint);
    }
    return result;
}
std::vector<PipelineHint> Hints::pipelines() const {
    std::lock_guard lock(mutex_);
    std::vector<PipelineHint> result;
    for (const auto &line : lines_) {
        PipelineHint hint;
        if (parse_pipeline_hint(line, &hint))
            result.push_back(hint);
    }
    return result;
}
bool Hints::flush() {
    std::lock_guard file_lock(file_mutex_);
    for (;;) {
        std::set<std::string> lines;
        uint64_t revision = 0;
        {
            std::lock_guard lock(mutex_);
            if (!dirty_)
                return true;
            lines = lines_;
            revision = revision_;
        }
        if (profile_.empty())
            return false;
        std::error_code error;
        std::filesystem::create_directories(profile_, error);
        if (error)
            return false;
        std::string temporary = profile_ + "/d3d9-warm-XXXXXX";
        const int fd = os_mkstemp(temporary.data());
        if (fd < 0)
            return false;
        FILE *file = (FILE *)os_fdopen(fd, "wb");
        if (!file) {
            os_fd_close(fd);
            os_unlink(temporary.c_str());
            return false;
        }
        bool ok = std::fputs("# d3d9 warm hints v1\n", file) >= 0;
        for (const auto &line : lines)
            ok = ok && std::fwrite(line.data(), 1, line.size(), file) == line.size() &&
                 std::fputc('\n', file) != EOF;
        ok = std::fclose(file) == 0 && ok;
        if (ok)
            ok = os_rename(temporary.c_str(), (profile_ + "/d3d9-warm.txt").c_str()) == 0;
        if (!ok) {
            os_unlink(temporary.c_str());
            return false;
        }
        std::lock_guard lock(mutex_);
        if (revision_ == revision) {
            dirty_ = false;
            damaged_ = false;
            return true;
        }
    }
}
} // namespace d9warm
