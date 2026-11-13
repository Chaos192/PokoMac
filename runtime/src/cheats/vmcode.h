// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Where the Atmosphère cheat VM's writes go (the engine's host, engine.cpp). A console's dmnt writes main's code through the kernel's debug
// write and the next instruction fetch runs it (ASM cheats: a branch over an instruction into a code cave in the padding after the module's
// code). Here guest code runs as compiled host code, so a byte written into code changes nothing by itself. The router splits each VM
// pass's writes by where they land:
//   - data (anything that is not a lifted function's code or the padding after a module's code): written at once, exactly as before;
//   - code (a word of a function the build lifted): held until the pass ends, then each changed word becomes a live code patch
//     (codepatch.h cp::apply_words: the function is lifted again from the patched bytes and swapped in at the next frame boundary, for
//     every call on a build lifted with --live-patch); a word written back to its original bytes reverts its patch;
//   - cave (the padding between a module's last instruction and the end of its last code page, where ASM cheats put their code): written
//     when the pass ends, before the code patches that branch there, and made code the dispatcher lifts from its bytes when it runs
//     (missed::code_region_add, as the overlay's Inject does); rewritten bytes drop the units made from the old ones.
// Each code or cave word remembers which cheats wrote it. Turning a cheat off reverts the words no other enabled cheat wrote (code back to
// its original bytes, the cave's bytes back to what they were): a console never reverts (the code stays as written until the game
// restarts), Breeze's restore-on-off does, and here a cheat switched off puts the game back.
// A pass that writes no code or cave (data cheats only) costs one range test per write more than before and ends with nothing to do; the
// counters show it (Counters::code_writes / cave_writes stay 0, no patch is made).
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nxrt::cheats::vmcode {

enum class Kind : uint8_t { Data, Code, Cave };

struct Place {
    Kind kind = Kind::Data;
    uint64_t lo = 0, hi = 0;           // Code: the lifted function [lo, hi); Cave: the module's padding [lo, hi)
};

// What the router needs from the runtime: the engine's live sink (vmcode_live.cpp: gmem, nxmodule, codepatch, missed) or a test's fake.
class Sink {
public:
    virtual ~Sink() = default;
    // Once per pass, before its first write: where code is now (modules can be loaded while the game runs).
    virtual void refresh() {}
    // True when [addr, addr + n) may touch code or a cave: the quick test every VM write takes. place() is asked only then.
    virtual bool maybe_code(uint64_t addr, size_t n) = 0;
    virtual Place place(uint64_t addr) = 0;
    virtual bool read(uint64_t addr, void* dst, size_t n) = 0;
    virtual bool write(uint64_t addr, const void* src, size_t n) = 0;
    // One-word live code patches; errs[i] empty when word i was patched, else why not.
    virtual void patch(const std::vector<std::pair<uint64_t, uint32_t>>& words, std::vector<std::string>* errs) = 0;
    virtual void unpatch(const std::vector<uint64_t>& words) = 0;
    // True when the code holding this word runs what memory now holds (no swap of its function is still on the way): a cave goes back only
    // when every code word that branched there has settled, else a thread could still enter it.
    virtual bool settled(uint64_t word) { (void)word; return true; }
    // True while the patch of this word is being made (lifted, compiled, what it reaches made ready) and not swapped in yet.
    virtual bool applying(uint64_t word) { (void)word; return false; }
    // True when the patch of this word could not be made (its function, or what it reaches, did not compile): it never went live, the game
    // runs the code it had. *why: the reason.
    virtual bool failed(uint64_t word, std::string* why) { (void)word; (void)why; return false; }
    // [lo, hi) of the cave [cave_lo, cave_hi) holds new bytes: make the cave code and drop what was made from the old bytes.
    virtual void cave_written(uint64_t cave_lo, uint64_t cave_hi, uint64_t lo, uint64_t hi) = 0;
    virtual void log(const std::string& line) { (void)line; }
};

struct Counters {
    uint64_t code_writes = 0;          // VM writes that landed in a lifted function's code
    uint64_t cave_writes = 0;          // VM writes that landed in a cave
    uint64_t passes_with_code = 0;     // passes that had code or cave bytes to settle
    uint64_t words_patched = 0;        // code words made live patches
    uint64_t words_reverted = 0;       // code words put back (a cheat off, its original written back, a reload)
    uint64_t patch_failures = 0;       // code words the patcher refused (then written as plain bytes, as before)
    uint64_t cave_words_written = 0;
    uint64_t cave_words_restored = 0;
};

// One word the VM changed and still holds.
struct Held {
    uint64_t addr = 0;
    Place place;                       // Code or Cave
    uint32_t original = 0;             // the bytes before the first cheat wrote it
    uint32_t now = 0;                  // what the cheats made it
    bool plain = false;                // code the patcher refused: written as plain bytes (as before the router), put back the same way
    std::string why;                   // plain: the patcher's reason
    std::set<int> owners;              // the cheat file's entries that wrote it (0 = the master)
};

class Router {
public:
    explicit Router(Sink& sink) : sink_(sink) {}

    void begin_pass();
    // A VM write by the cheat file's entry `entry`. Data is written now (the Sink's write); code and cave bytes wait for end_pass.
    // False only when a data write failed.
    bool write(uint64_t addr, const void* src, size_t n, int entry);
    // True when [addr, addr + n) may be code or a cave (the engine routes only those writes here; the others go straight to memory).
    bool maybe_code(uint64_t addr, size_t n) { return sink_.maybe_code(addr, n); }
    // A VM read: memory, with this pass's code and cave bytes that are not written yet laid over it.
    bool read(uint64_t addr, void* dst, size_t n);
    bool pending() const { return !pend_.empty(); }
    // Settles the pass: cave bytes first, then the code patches (and reverts of words written back to their original bytes).
    void end_pass();

    // A cheat was turned off: its words that no other enabled cheat wrote go back. on: it may write again from the next pass.
    void entry_off(int entry);
    void entry_on(int entry);
    // Every word back (the cheat file was reloaded: its entries may be other cheats now).
    void reset();

    Counters counters() const;
    std::vector<Held> held() const;
    // The cheat's code is on its way in (the cheats list says "applying..."): a code word it holds is not swapped in yet.
    bool applying(int entry) const;
    // The cheat's code cannot go live: a code word it holds could not be made (Sink::failed) or the patcher refused it. *why: the reason.
    // The engine turns such a cheat off and the cheats list shows why.
    bool failure(int entry, std::string* why) const;
    size_t caves_waiting() const;                      // cave words whose return waits for their code to settle

private:
    // mu_ held: the words go back (code unpatched by the queued work; cave words once the code reverted with them has settled: after the
    // unpatch, or at a later pass while its function compiles), records dropped
    void restore_locked(const std::vector<uint64_t>& words);
    void settle_caves_locked();                        // the waiting cave words whose code has settled go back (queued work)

    // The sink work is decided under mu_ and done after it is let go, one caller at a time (work_mu_): a sink call can wait for the on-demand
    // compiler (cave_written drops the units of the old bytes under missed's make lock, which a compile holds for a second or more), and the
    // cheats overlay reads this router every frame (applying, held, counters) and turns cheats on and off from the render thread: with mu_
    // held through that wait, the frame waited too (Pokopia: 217-293 ms frames when a cheat's cave was written while another
    // cheat's code was compiling).
    struct Op {
        enum class K : uint8_t { Write, Cave, Unpatch, Patch } k = K::Write;
        uint64_t a = 0, b = 0, c = 0, d = 0;           // Write: a = word; Cave: cave_written(a, b, c, d)
        uint32_t v = 0;                                // Write: the word's value
        std::vector<uint64_t> words;                   // Unpatch
        std::vector<std::pair<uint64_t, uint32_t>> patch;   // Patch
    };
    void run_ops_held();                               // work_mu_ held: the queued work, in order, and the caves it lets settle
    void run_ops(std::vector<Op>& ops, bool* unpatched);
    void run_ops_now_or_later();                       // a toggle's work: now when no pass holds work_mu_, else by that pass
    void end_pass_locked();                            // mu_ held: end_pass's decisions, its sink work queued
    static Op write_op(uint64_t w, uint32_t v);
    static Op cave_op(uint64_t cave_lo, uint64_t cave_hi, uint64_t lo, uint64_t hi);
    std::vector<Op> ops_;                              // under mu_
    std::mutex work_mu_;

    struct CaveBack {
        Held word;
        std::vector<uint64_t> wait_for;                // code words reverted with it
    };
    Sink& sink_;
    // this pass (the engine's thread only)
    struct PendWord {
        Place place;
        std::map<int, std::pair<uint8_t, uint32_t>> by;   // entry -> the bytes of its last write here (mask of the 4, their values)
    };
    std::map<uint64_t, uint8_t> pend_;                 // byte -> its value now
    std::map<uint64_t, PendWord> pend_words_;          // word -> its place and who wrote what
    uint64_t pass_gen_ = 0;                            // gen_ when the pass began
    // across passes
    mutable std::mutex mu_;
    std::map<uint64_t, Held> held_;                    // by word address
    std::map<uint64_t, CaveBack> caves_back_;          // cave words to put back once their code has settled
    std::set<int> revoked_;                            // turned off since the pass in flight began: its writes are dropped
    uint64_t gen_ = 0;                                 // bumped by reset: a pass begun before it is dropped
    Counters c_;
};

// The engine's sink over the running game (vmcode_live.cpp).
Sink& live_sink();

}  // namespace nxrt::cheats::vmcode
