// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The cheat VM's write router over the running game (vmcode.h): where code is (the registered lifted modules), memory through gmem, code
// words through the live code patcher (codepatch.h), caves made code the dispatcher lifts from their bytes (missed.h code regions).
#include <mutex>
#include <set>

#include "codepatch.h"
#include "gmem.h"
#include "nxrt/log.h"
#include "nxrt/missed.h"
#include "nxrt/nxmodule.h"
#include "vmcode.h"

namespace nxrt::cheats::vmcode {
namespace {
constexpr const char* kTag = "cheats";

class LiveSink : public Sink {
public:
    void refresh() override {
        mods_.clear();
        for (const NxModule* nx : nxmodules()) {
            if (!nx || nx->text_end <= nx->base) continue;
            // the cave: the padding after the module's last instruction up to its last 4 KiB code page (cp::inject's cave)
            mods_.push_back({nx->base, nx->text_end, (nx->text_end + 0xfffull) & ~0xfffull});
        }
    }
    bool maybe_code(uint64_t addr, size_t n) override {
        for (const M& m : mods_)
            if (addr + n > m.base && addr < m.cave_end) return true;
        return false;
    }
    Place place(uint64_t addr) override {
        for (const M& m : mods_) {
            if (addr < m.base || addr >= m.cave_end) continue;
            if (addr >= m.text_end) return {Kind::Cave, m.text_end, m.cave_end};
            uint64_t s = 0, e = 0;
            if (cp::function_at(addr, nullptr, &s, &e)) return {Kind::Code, s, e};
            return {};                                   // in the text but not code the build lifted: a plain write, as before
        }
        return {};
    }
    bool read(uint64_t addr, void* dst, size_t n) override { return gmem::read(addr, dst, n); }
    bool write(uint64_t addr, const void* src, size_t n) override { return gmem::write(addr, src, n); }
    void patch(const std::vector<std::pair<uint64_t, uint32_t>>& words, std::vector<std::string>* errs) override {
        const size_t n = cp::apply_words(words, errs);
        NXLOG_I(kTag, "cheat code writes: %zu of %zu words made live code patches", n, words.size());
    }
    void unpatch(const std::vector<uint64_t>& words) override {
        const size_t n = cp::revert_words(words);
        NXLOG_I(kTag, "cheat code writes: %zu of %zu words back to their original code", n, words.size());
    }
    bool settled(uint64_t word) override { return cp::settled(word); }
    bool applying(uint64_t word) override {
        for (const cp::Patch& p : cp::patches())
            if (p.addr == word) return p.state == cp::State::Compiling;
        return false;
    }
    bool failed(uint64_t word, std::string* why) override {
        for (const cp::Patch& p : cp::patches())
            if (p.addr == word) {
                if (p.state != cp::State::Failed) return false;
                if (why) *why = p.note;
                return true;
            }
        return false;
    }
    void cave_written(uint64_t cave_lo, uint64_t cave_hi, uint64_t lo, uint64_t hi) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!caves_.count(cave_lo)) {
            missed::code_region_add(cave_lo, cave_hi - cave_lo, "cheat cave");
            caves_.insert(cave_lo);
        }
        missed::code_region_invalidate(lo, hi - lo);       // a unit made from the old bytes goes; the next entry lifts these
        NXLOG_I(kTag, "cheat cave at %s: %llu bytes changed (code lifted from these bytes when it runs)", gmem::describe(lo).c_str(),
                static_cast<unsigned long long>(hi - lo));
    }
    void log(const std::string& line) override { NXLOG_W(kTag, "%s", line.c_str()); }

private:
    struct M { uint64_t base, text_end, cave_end; };
    std::vector<M> mods_;                                // refresh() (the engine's thread)
    std::mutex mu_;
    std::set<uint64_t> caves_;                           // caves made code regions
};
}  // namespace

Sink& live_sink() {
    static LiveSink s;
    return s;
}

}  // namespace nxrt::cheats::vmcode
