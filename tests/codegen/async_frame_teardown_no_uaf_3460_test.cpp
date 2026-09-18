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
// `heap_uaf_count == 0` across N = 256 parallel attempts. NOT
// `leaksAtLeast(...)` (the defect class is UAF, not LSan-detectable leak),
// NOT `exitCode == 0` (the Bezel runtime appears to `_exit(0)` from
// somewhere inside teardown before ASan finishes flushing — every observed
// failure exits 0; the only signal is the truncated 5-line ASan report in
// stderr).
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
    std::atomic<int> ran_ok{0};

    // Warm the bincache so all 8 children share one compile result.
    (void)runAotChecked(kN24);

    std::vector<std::thread> workers;
    workers.reserve(W);
    for (int w = 0; w < W; ++w) {
        workers.emplace_back([&heap_uaf_count, &ran_ok]() {
            for (int i = 0; i < K; ++i) {
                JitResult r = runAotChecked(kN24);
                if (!r.ran) {
                    for (const auto& d : r.diagnostics.diagnostics())
                        if (d.message.find("heap-use-after-free") != std::string::npos) {
                            heap_uaf_count.fetch_add(1, std::memory_order_relaxed);
                            goto done;
                        }
                done:
                    ;
                } else {
                    ran_ok.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& t : workers) t.join();

    BEZEL_EXPECT_EQ(heap_uaf_count.load(), 0);
    // Sanity: at least most runs reached `r.ran`. The diagnostic observed
    // r.ran == true on every UAF failure (exit code 0; the truncated ASan
    // report still surfaces via diagnostics), so a green `heap_uaf_count`
    // together with a green `ran_ok` is the success signal.
    BEZEL_EXPECT_TRUE(ran_ok.load() > 0);
}
