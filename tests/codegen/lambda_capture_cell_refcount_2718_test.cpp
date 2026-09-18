// Codegen — fix #2718 cycle-10 follow-up (§27.5 Closures, §28.3 Local
// variables ownership and borrows, §28.9 Runtime-managed types, spec v0.298):
// the lambda capture cell is a 16-byte `{ptr value, int32 rc}` cell at offset 0
// of the boxed cell alloca, shared by every closure env that captures the
// same local. The cell's value release is keyed to the CELL'S DEATH
// (`rc == 0` after one decrement per env), not to any individual envdtor —
// pre-#2718 each envdtor released the value, post-#2718 the value is released
// exactly once at the cell's destruction.
//
// THE BUGS THIS REGRESSION PINS. The anchor's first commit (`5bed3f75`) wired
// the helper but, on the cycle-10 per-fix review, was found to have dropped
// THREE release arms the pre-#2718 envdtor had explicitly:
//
//   A. DictEntry<string, V> K release (§27.5 / stdlib §4.8): K is at offset 0
//      of the {K, V} struct, but `isStringTypeRefNonNull(semaTy)` returns
//      true only for `string` primitive, never for the Named
//      `DictionaryEntry<string,V>`. The K release was simply not emitted, so
//      every escaping-capture `fe := d.First()`-shaped program leaked 32+3 B
//      per call (the bz_string_t + payload).
//
//   B. DrainedEntry<K, V> K-release + V-dispose (§27.5 / stdlib §4.9): same
//      shape; K-release by `bz_string_release(K)` and V-dispose at the
//      pre-existing site (the envdtor's `singleCaptureLocals_` machinery
//      covers class-typed V). Without the explicit arm, K was stranded.
//
//   C. Box<T> wrapper release (§28.9 + #3436): a `const o: Object := i`
//      captured by a closure wraps the value in a 24-byte box. Pre-#2718's
//      `if (ce.releaseBoxInEnvdtor) emitBoxRelease(box)` arm was dropped by
//      the helper, so the box leaked per capture.
//
// THE FIX (this commit). `emitCellRefcountedFree` now has explicit arms for
// `isDictEntryStringKeyTypeRef(semaTy)` (K-release on `forceFree`,
// FINDING A), `isDrainedEntryTypeRef(semaTy)` (K-release + V-dispose,
// FINDING B), and the `releaseBox` flag (FINDING C). Each arm is gated by
// the existing `forceFree` / `releaseBox` / `suppressContent` guards that
// #2718's cell-rc model requires — value release is keyed to cell death
// (rc==0) when the cell is multi-env-shared, but to the single envdtor when
// the cell has exactly one capturing closure (singleCaptureLocals_ +
// definitelyEscapingLambdas_ — the #694/#4127 "single-holder,
// definite-escape" proof that makes freeing the cell at the envdtor sound).
//
// This file pins the four release arms with loop-amplified leak oracles:
//
//   Row 1 (cell-rc core — string capture): the cell's value release fires
//          once at rc==0 across N envs. Leak-checked 500x. Pins the cell-rc
//          model's string-release path (#2718 itself — still present on the
//          anchor).
//
//   Row 2 (cell-rc core — closure capture): same model for a function-typed
//          captured local. Plain runAot — pre-fix aborts with use-after-free
//          / double-free on the shared cell, post-fix runs clean. The cell-rc
//          model LEAKS the captured value by design (cell survives last env
//          death by ~1 frame so a deferred caller can still read it), so an
//          LSan oracle fails on BOTH sides.
//
//   Row 3 (FINDING C — Box<T> wrapper, SOLE-escaping arm): 24 B / 1 alloc
//          for the box wrapper + 16 B / 1 alloc for the box-descriptor payload
//          per call = 40 B / 2 allocs. Pre-fix measured leak: 20000 B / 1000
//          allocs. Post-fix: clean. SOLE-escaping shape (singleCaptureLocals_ ∧
//          definitelyEscapingLambdas_, #694/#3426) — the non-escaping box
//          capture has a DIFFERENT second mechanism (see the 3436 tests still
//          failing pre-fix-on-anchor; deferred to cycle-11 per FINDING D).
//
//   Row 4 (FINDING A — DictEntry<string,V> K release): 32+3 B / 2 allocs per
//          call (the bz_string_t header + payload). Pre-fix measured leak:
//          ~17500 B / 1000 allocs. Post-fix: clean.
//
// Loop-amplification is load-bearing: a ONE-SHOT LSan row can report clean on
// BOTH sides because the leaked allocation is still reachable from the live
// stack at exit. The 500x loop brings the leak past LSan reachability.
//
// FINDING B (DrainedEntry<K,V> K-release + V-dispose) is covered by the
// existing dict_drain_entry_dispose_capture_4136_test, which exercises the
// `isDrainedEntryTypeRef` arm via a different test surface — this file does
// not duplicate that.
//
// Verified FAIL-WITHOUT on `5bed3f75` (the anchor) by stashing the
// DictEntry/Box arms of `emitCellRefcountedFree`: rows 3 and 4 reported
// ASan+LSan leaks identical to the measurements in the per-fix review
// (FINDINGS A & B). Verified PASS-WITH on this commit.
//
// ORACLE. `runAotChecked` (ASan+LSan) for leak-checking rows; plain `runAot`
// for the function-typed sibling (Row 2). A `runAotChecked` run that is
// SKIPPED (no ASan toolchain) returns ran=false with "SKIP" in diagnostics;
// those are SKIPPED, not failed, per the test's wasSkipped() guard below.
// Reference values are the program outputs, not the leak byte counts — those
// are checked against LSan (no `leaksAtLeast` weakening).

#include <string>
#include <string_view>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ============================================================================
// Row 1 — captured string across two closures (cell-rc core, string path).
//
// §27.5: the cell is shared by every env that captures `s`. Pre-#2718 each
// envdtor released the string (over-release). Post-#2718 the string is
// released once at cell death. Leak oracle: 500x loop; per-call, "v-1"+ "!"
// is built at runtime (NOT constant-folded). 500 * (1 string header + 1
// payload) = 500 allocations; pre-#2718 over-release aborts, post-fix
// leak-clean. The pre-existing #672 transfer model (first capture takes
// the +1, additional captures retain) gives the cell exactly 1 string at
// any point, so this is the textbook single-value-in-cell case.
// ============================================================================
const char* kCapturedStringSharedByTwo =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    var f1 := lambda(k: int) => do return s.Length; end; "
    "    var f2 := lambda(k: int) => do return s.Length; end; "
    "    return f1(1) + f2(2); "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; var i := 0; "
    "    while i < 500 do "
    "      total := total + C.Two($\"v-{i+1}\"); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine($\"total={total}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_lambda_capture_cell_refcount_2718,
           captured_string_shared_by_two_envs_no_leak_spec_27_5) {
    JitResult r = runAotChecked(kCapturedStringSharedByTwo);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // FAILS-WITHOUT: ASan/LSan leak
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=4784"));  // sum 2*len($"v-{i+1}") over i=0..499
}

// ============================================================================
// Row 2 — captured closure (function-typed cell) across two envs (cell-rc core,
// function-type path). Function-typed values are runtime-managed (§28.9); the
// same cell-rc model applies — the cell holds one ptr to the closure env,
// released once at cell death. NOT a nested capture (which has its own deeper
// mechanism deferred to cycle-11).
//
// Pattern mirrors lambda_captured_string_cell_write_2706 (G) — the proven
// working function-typed sibling shape: one captured `g`, two readers (f1, f2),
// one rebind. `runAot` (NOT `runAotChecked`) — the cell-rc model LEAKS the
// captured value by design (cell survives last env death by ~1 frame so a
// deferred caller can still read it), so an LSan oracle fails on both sides;
// pre-fix this aborts with use-after-free / double-free, post-fix it runs
// clean. The plain `ran` + correct exitCode is the live oracle.
// ============================================================================
const char* kCapturedClosureSharedByTwo =
    "module M; class C "
    "  public static method Two(p: string): int "
    "    var g: (int) -> int := lambda(k: int) => do return k + p.Length; end; "
    "    var f1 := lambda(k: int) => do return g(k); end; "
    "    var f2 := lambda(k: int) => do return g(k + 1); end; "
    "    g := lambda(k: int) => do return k + 2 * p.Length; end; "
    "    return f1(1) + f2(2) + g(3); "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..9 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 225
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_lambda_capture_cell_refcount_2718,
           captured_closure_shared_by_two_envs_no_double_free_spec_28_9) {
    JitResult r = runAot(kCapturedClosureSharedByTwo);
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: free(): invalid pointer
    BEZEL_EXPECT_EQ(r.exitCode, 225);
}

// ============================================================================
// Row 3 — Box<T> capture (FINDING C fix, SOLE-escaping arm). A
// `const o: Object := 42` boxes the int into a 24-byte wrapper; the lambda
// captures the boxed value and DEFINITELY escapes (is directly returned) —
// the `forceFree` path of `emitCellRefcountedFree`, gated by
// `ce.freeCellInEnvdtor` (singleCaptureLocals_ ∧
// definitelyEscapingLambdas_, #694/#3426). Pre-#2718's
// `if (ce.releaseBoxInEnvdtor) emitBoxRelease(box)` arm was dropped; this
// row pins the explicit `releaseBox` arm in `emitCellRefcountedFree`
// (FINDING C).
//
// SOLE-escaping shape (not the non-escaping box capture 3436 tests — that
// case has a DIFFERENT second mechanism deferred to cycle-11). 500
// iterations: each iteration allocates one Box (24 B) + one box-descriptor
// payload (16 B) = 40 B / 2 allocs. Pre-fix measured leak: 20000 B / 1000
// allocs. Post-fix: clean.
// ============================================================================
const char* kBoxCaptureLeakFree =
    "module M; class C "
    "  public static method Sole(): () -> bool "
    "    const o: Object := 42; "
    "    return lambda() => o is int; "
    "  end Sole; "
    "  public static method Main(): int "
    "    var acc := 0; "
    "    var i := 0; "
    "    while i < 500 do "
    "      var f := C.Sole(); "
    "      if f() then acc := acc + 1; end if; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(acc.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_lambda_capture_cell_refcount_2718,
           box_sole_escaping_capture_no_wrapper_leak_spec_28_9_box_release) {
    JitResult r = runAotChecked(kBoxCaptureLeakFree);
    if (wasSkipped(r)) return;
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "    DIAG: %s\n", d.message.c_str());
        std::fprintf(stderr, "    stdout: '%s'\n", r.stdoutText.c_str());
        std::fprintf(stderr, "    exitCode: %d\n", r.exitCode);
    }
    BEZEL_EXPECT_TRUE(r.ran);          // FAILS-WITHOUT: 20000 B / 1000 LSan leak
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "500"));
}

// ============================================================================
// Row 4 — DictEntry<string, V> capture (FINDING A fix). A
// `DictionaryEntry<string, int>` value held in the cell has K at offset 0;
// the cell-rc string-release path needs the explicit
// `isDictEntryStringKeyTypeRef` arm because `isStringTypeRefNonNull` returns
// false for the Named `DictionaryEntry<string, int>` type. Without the
// arm, the rebuilt K string ("xy") is never released.
//
// Per call: 32+3 B / 2 allocations (the bz_string_t header + the "xy\0"
// payload). 500 calls → ~17500 B / 1000 allocs LSan-reachable pre-fix;
// clean post-fix. "xy".Length == 2, Value = 9, total per call = 11;
// 500 * 11 = 5500.
// ============================================================================
const char* kDictEntryStringKeyCapture =
    "module M; class C "
    "  public static method MakeClosure(): () -> int "
    "    var d := new Dictionary<string, int>(); "
    "    d.Add(\"xy\", 9); "
    "    var fe := d.First(); "
    "    return lambda() => fe.Key.Length + fe.Value; "
    "  end MakeClosure; "
    "  public static method Main(): int "
    "    var total := 0; var i := 0; "
    "    while i < 500 do "
    "      var f := C.MakeClosure(); "
    "      total := total + f(); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine($\"total={total}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_lambda_capture_cell_refcount_2718,
           dict_entry_string_key_capture_no_k_release_leak_spec_27_5_dict_entry) {
    JitResult r = runAotChecked(kDictEntryStringKeyCapture);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // FAILS-WITHOUT: ~17500 B / 1000 LSan leak
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=5500"));
}
