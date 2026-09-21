// Codegen — Phase 0 code item #3460 (defect-campaign §1.1): an intermittent
// `heap-use-after-free in bz$async$M.C$Fill$Resume`, byte-identical stack to
// the original 3-of-490 report on `origin/main = 1e3861a`.
//
// WHAT THIS TEST PINS.
// The race is alive on HEAD `b57a712e` at ~1.7 % under parallel load
// (measured by the diagnostic lane: 19 / 1100 ≈ 1.7 %, every failure
// stack-identical to the original — `pc=0x4ef7b8`, read of size 8 inside
// `bz$async$M.C$Fill$Resume`, thread T1). The reproducer is the issue's
// own shape: an async method that assigns to a class field lambda
// (`this._h := lambda() => k;`) AFTER an `await Task.Delay(0)` so the
// `Resume` continuation runs in the worker thread after the caller's
// `using var c` scope has dropped the instance.
//
// WHY A PARALLEL ORACLE.
// Per the diagnostic, sequential runs (single-process, one fork at a time)
// NEVER reproduce the race (0 / 640 = 0 % across batches 1-5 of the
// diagnostic). The race needs thread-pool contention to fire: the worker
// pool in one process must be draining while another process's caller is
// disposing its captured state. This test therefore spawns W = 8 parallel
// child processes (fork+exec), each running the binary K times, for a total
// of N = W * K = 8 * 32 = 256 attempts. Under the same parallel-load shape
// the diagnostic used (xargs -P 8 to -P 16), this gives the worker pool
// enough cross-process contention to fire the race at the diagnostic's
// measured ~1-3 % rate. At 1.7 %, 256 attempts gives ~4.4 expected UAFs;
// the test FAILs if any are observed.
//
// ORACLE INTEGRITY (LANE_RULES §"oracle strength").
// Every ASan CORRUPTION headline the N = 256 parallel attempts produce must
// be zero — `heap-use-after-free`, `double-free`, and the generic
// `ERROR: AddressSanitizer` catch-all. NOT `exitCode == 0` (the Bezel runtime
// appears to `_exit(0)` from somewhere inside teardown before ASan finishes
// flushing — every observed failure exits 0; the only signal is the ASan
// report `runAotChecked` copies into the diagnostics).
//
// fix #4540 (§28.3, v0.306) WIDENED THIS ORACLE, and the widening is the
// point of that issue as much as the codegen change is. As first written the
// row counted `heap-use-after-free` ONLY, and only inside an `if (!r.ran)`
// arm. The #3460 residual of #4540 is a DOUBLE-FREE, produced by THIS test's
// own reproducer on THIS test's own threads — four of them in the `--full`
// ASan pass of gate `e89577e4`, stack
// `bz_closure_release <- bz$async$M.C$Fill$Resume <- domain_drain` — and this
// row reported `[ OK ]` for every one of them, because `double-free` matched
// no substring anyone was counting. An oracle that names one spelling of
// corruption certifies one spelling of corruption.
//
// The LEAK leg is measured and asserted too. Pre-#4540 the reproducer leaked
// `68 byte(s) in 3 allocation(s)` in 1,972 of 2,000 runs: the owner's
// `DisposeCore` released `_h` and NULLED the field before waiting for the
// worker's borrow, so the worker's `this._h := lambda() => k;` then stored a
// closure into a field nobody would ever release. That leak and the
// double-free are the two interleavings of ONE placement bug, so a row that
// tolerated the leak would have kept 98.6 % of the evidence off the record.
//
// SPEC CITATION.
// §28.3 ("Local variables, ownership, and borrows") — "the borrow of the
// captured state must outlive any pending resumption"; §30.1 ("Async
// methods"); §30.5 ("Result lifetime and Task disposal"); §30.13 ("Task
// scheduling and continuations"). The race is an instance of the borrow
// outliving claim being violated when the caller's scope-end disposal
// frees the captured `outerTask` reference (frame + 0x38) before the
// worker thread reads it to call `bz_task_complete`.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan AOT harness
#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// Reproducer source. md5 `e63e92bd4c2b748dcc51951691087c78` — preserved
// byte-identical to the diagnostic lane's copy. The diagnostic showed the
// #2434 degradation on line 20 (`lowerLocalDecl's initializer expression
// failed to lower to a value for this local's slot (degraded to a
// zero-initialized value) — #2434`); that degradation is the entry
// condition for the race (the caller does not actually block on the
// `outerTask`, so the caller's scope-end disposal can race the worker's
// resume). Per the brief: #2434 is closed upstream, but the race is
// alive on this HEAD — so the fix path is the teardown-order side, not
// the degradation side.
static const char* kN24 =
    "module M; import System; import System.Threading.Tasks; "
    "class C "
    "  private _h: () -> int := lambda() => 0; "
    "  public async method Fill(u: int): Task<int> "
    "    var k: int := u; "
    "    var e := lambda() => k; "
    "    var s: int := e(); "
    "    await Task.Delay(0); "
    "    this._h := lambda() => k; "
    "    return s; "
    "  end Fill; "
    "  public method Call(): int "
    "    return this._h(); "
    "  end Call; "
    "end C; "
    "class D "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    var t := c.Fill(7); "
    "    var s: int := t.GetAwaiter().GetResult(); "
    "    Console.WriteLine(\"s=\" + s.ToString()); "
    "    Console.WriteLine(\"h=\" + c.Call().ToString()); "
    "    return 0; "
    "  end Main; "
    "end D;";

namespace {

}  // namespace

// =====================================================================
// Primary oracle row: the reproducer compiled under ASan, run K times
// each by W parallel child processes, for N = W * K = 8 * 32 = 256
// total attempts. The parallel shape (W children forked from one
// parent, all running concurrently) is what reproduces the diagnostic's
// 1.7 % race rate; the diagnostic measured 0 % under purely sequential
// runs (0 / 640 across batches 1-5), and the brief's "single-process
// sequential rate is lower but non-zero" is REFUTED by the diagnostic
// data. The oracle is `heap_uaf_count == 0`. Pre-fix: count > 0 at
// N = 256 (parallel-load rate ~1.7 %, expect ~4 UAFs). Post-fix:
// count == 0.
//
// Per the brief: the per-fix review agent should re-measure at N = 1100
// (the diagnostic's full sample) before promoting the fix to the gate.
// This gtest is the in-tree regression pin; the full measurement is
// done by `scripts/measure_3460.sh` per the brief.
//
// per §28.3, §30.1, §30.5, §30.13 (issue #3460)
BEZEL_TEST(codegen_async_frame_teardown_no_uaf_3460,
           worker_resume_does_not_read_freed_outer_task_under_asan_spec_28_3) {
    [[maybe_unused]] constexpr int W = 8;        // parallel worker count
    [[maybe_unused]] constexpr int K = 32;       // attempts per worker
    // total = W * K = 256 attempts

    std::atomic<int> heap_uaf_count{0};
    // fix #4540: the three counters the original row did not have.
    std::atomic<int> double_free_count{0};
    std::atomic<int> asan_error_count{0};
    std::atomic<int> leak_count{0};
    std::atomic<int> ran_ok{0};

    // Warm the bincache so all 8 children share one compile result.
    (void)runAotChecked(kN24);

    std::vector<std::thread> workers;
    workers.reserve(W);
    for (int w = 0; w < W; ++w) {
        workers.emplace_back([&heap_uaf_count, &double_free_count,
                              &asan_error_count, &leak_count, &ran_ok]() {
            for (int i = 0; i < K; ++i) {
                JitResult r = runAotChecked(kN24);
                // fix #4540: scan the diagnostics UNCONDITIONALLY. The `!r.ran`
                // guard this replaces was not a filter, it was a blindfold:
                // `runAotChecked` reports a fired sanitizer as ran=false WITH
                // the report text, so gating the scan on `!r.ran` only ever
                // skipped attempts that had nothing to report anyway — while
                // making the row read as if a clean run could hide corruption.
                // Counting per-attempt (a run is one hit at most per class)
                // keeps the numbers comparable to the N above.
                bool sawUaf = false, sawDoubleFree = false;
                bool sawAsan = false, sawLeak = false;
                for (const auto& d : r.diagnostics.diagnostics()) {
                    if (d.message.find("heap-use-after-free") != std::string::npos)
                        sawUaf = true;
                    if (d.message.find("double-free") != std::string::npos)
                        sawDoubleFree = true;
                    if (d.message.find("ERROR: AddressSanitizer") != std::string::npos)
                        sawAsan = true;
                    if (d.message.find("ERROR: LeakSanitizer") != std::string::npos)
                        sawLeak = true;
                }
                if (sawUaf) heap_uaf_count.fetch_add(1, std::memory_order_relaxed);
                if (sawDoubleFree)
                    double_free_count.fetch_add(1, std::memory_order_relaxed);
                if (sawAsan) asan_error_count.fetch_add(1, std::memory_order_relaxed);
                if (sawLeak) leak_count.fetch_add(1, std::memory_order_relaxed);
                if (r.ran) ran_ok.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : workers) t.join();

    BEZEL_EXPECT_EQ(heap_uaf_count.load(), 0);
    // fix #4540 (§28.3, v0.306): the double-free leg. This is the spelling the
    // four reports in gate `e89577e4` carried, and the one this row used to
    // pass straight through.
    BEZEL_EXPECT_EQ(double_free_count.load(), 0);
    // fix #4540: the catch-all, so the NEXT spelling of corruption (a
    // heap-buffer-overflow, a SEGV on an unknown address) does not need
    // another gate cycle to be noticed.
    BEZEL_EXPECT_EQ(asan_error_count.load(), 0);
    // fix #4540: the leak leg — the common interleaving of the same bug.
    BEZEL_EXPECT_EQ(leak_count.load(), 0);
    // Sanity: at least most runs reached `r.ran`. Left at the original `> 0`
    // DELIBERATELY. It is tempting to tighten it to `W * K` now that the leak
    // is gone, but `r.ran` is also false for a harness TIMEOUT or an infra
    // failure, neither of which is a memory-safety finding — and this row runs
    // 256 forked compiles+execs inside a contended gate. The four counters
    // above carry the whole memory-safety verdict and none of them can be
    // moved by contention, so tightening this one would only buy flake.
    BEZEL_EXPECT_TRUE(ran_ok.load() > 0);
}
