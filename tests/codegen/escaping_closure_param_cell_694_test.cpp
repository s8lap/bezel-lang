// Codegen / leak — an ESCAPING (returned) closure that captures a PARAMETER
// leaked its capture cell. Issue #694 (residue-2, the escaping-closure
// capture-cell half).
//
// Spec: §28.9 (runtime-managed types — "Function-typed values … the
// heap-allocated closure object that backs the function value" is reclaimed by
// the runtime, so nothing the closure allocates may outlive it), §27.5
// (lambdas and captures — a captured variable's storage is promoted to a shared
// heap cell), §28.3 (captured owned bindings are borrows). Spec revision v0.228.
//
// THE BUG. Capturing a variable promotes its storage to a heap "capture cell"
// (`boxLocalSlot`). For a closure that DEFINITELY escapes its defining scope —
// syntactically `return () => …`, a `return f` naming a never-reassigned var
// initialized from a lambda, or a store into `this.f` assigned exactly once —
// the scope-end cell free cannot run (it would UAF the escaped env, which holds
// a raw pointer into the cell), so the cell is instead freed by the closure's
// per-lambda ENVDTOR at last-drop (#672's typed env disposal). Arming that free
// requires the captured entity to be in `singleCaptureLocals_` — captured by
// exactly one env, so the envdtor is the cell's only free.
//
// `collectBoxCaptureLocals` built that set by tallying captures per
// `sema::LocalSymbol`, keyed by the symbol's AST `LocalDecl`. A captured
// PARAMETER has no `LocalDecl` — `lowerLambda`'s captured-param branch keys its
// cell by the `sema::Symbol*` itself — so parameters were never tallied and
// never entered `singleCaptureLocals_`. `return () => param` therefore never
// armed the envdtor cell-free and leaked one cell per returned closure:
// 4 bytes for an `int` capture, 8 bytes for a `string` reference. The issue's
// recorded measurement — 51 cells / 408 B for 51 returned closures — is exactly
// 51 × 8 B of string-parameter capture cells; `fifty_one_returned_closures_*`
// below pins that number. Capturing a LOCAL was already clean, which is why the
// gap survived: copying the parameter into a local (`var x := n;`) is a
// complete workaround, and the leak only shows on the parameter path.
//
// THE FIX (two halves, both required):
//   (1) collectBoxCaptureLocals (LlvmBackend_Setup.cpp) also tallies captured
//       `sema::ParameterSymbol`s, keyed by `Symbol*` — the key lowerLambda
//       already uses for a parameter's cell. `boxCaptureLocals_` is unaffected
//       (it additionally requires membership in `candidates`, which holds only
//       `LocalDecl` nodes), so #802's box rc-retain model is untouched.
//   (2) lowerLambda's captured-PARAMETER branch (LlvmBackend_Exprs2.cpp) arms
//       `ce.freeCellInEnvdtor` under exactly the guard the captured-LOCALS
//       branch uses: sole holder (`singleCaptureLocals_`) AND definitely
//       escaping (`definitelyEscapingLambdas_`) AND freshly boxed AND not a
//       guarded (loop-reused) cell.
//
// WHY THE GUARD IS THE SOUND ONE (the hazard this issue warns about). Freeing
// the cell at scope exit for an escaping closure is a use-after-free — the env
// points at the cell. `definitelyEscapingLambdas_` is an UNDER-approximation of
// escape (only the three syntactic forms above), never the conservative
// `capturedByEscaping` over-approximation, which over-includes genuinely
// non-escaping closures whose envdtor runs AT scope end (freeing there would UAF
// the outer scope's later access through the same cell — the FIRING-22 finding
// recorded on the issue). Sole-holder rules out a double-free. The scope-end
// `boxedCellFree` cannot also fire: `collectCellFreeEligible` excludes anything
// captured by an escaping lambda, and a returned lambda is always classified
// escaping there.
//
// OBSERVABILITY. runAotChecked links + runs the generated code under ASan/LSan,
// so the leaked cell flips r.ran to false; the exit code pins the captured
// value, so a premature free (UAF) corrupts the result as well as flipping
// r.ran. Every `*_lsan_clean_694` test FAILS without the fix (LSan reports the
// leaked capture cell) and PASSES with it. The two canaries pin the excluded
// cases: they must not crash and must stay value-correct, and use the LENIENT
// checked run because their (deliberate, bounded) cell leak is the safe
// direction.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent:
// runAotChecked returns ran=false with a "SKIP" diagnostic. clang is required to
// build this repo, so a skip is not expected — guard anyway.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

void expectCleanValue(const std::string& src, int expected) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, expected);
}

// Value + memory-safety only (no leak detection): for the deliberately-excluded
// shapes, whose cell leak is the recorded safe direction. A double-free / UAF
// still flips r.ran.
void expectSafeValue(const std::string& src, int expected) {
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, expected);
}

// (A) The #694 repro, int parameter: a directly-returned lambda capturing the
// method's `int` parameter. Pre-fix LSan: 4 bytes leaked in 1 allocation (the
// capture cell). Returns 7.
const char* kReturnLambdaIntParam =
    "module M; class C "
    "  public static method Make(n: int): () -> int "
    "    return lambda() => n; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f: () -> int := C.Make(7); "
    "    return f(); "   // 7
    "  end Main; "
    "end C;";

// (B) The #694 repro, string parameter — the 8-byte (pointer-sized) cell that
// the issue's 408 B / 51 closures measurement is made of. Pre-fix LSan: 8 bytes
// leaked in 1 allocation. The string itself was already released by the
// envdtor's string release, which runs BEFORE the cell free (the free must not
// precede the read through the cell). Returns "abcd".Length = 4.
const char* kReturnLambdaStringParam =
    "module M; class C "
    "  public static method Make(s: string): () -> int "
    "    return lambda() => s.Length; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f: () -> int := C.Make(\"abcd\"); "
    "    return f(); "   // 4
    "  end Main; "
    "end C;";

// (C) TWO parameters captured by one returned lambda — each gets its own cell,
// each held by this single env, so both are freed. Pre-fix LSan: 8 bytes in 2
// allocations. Returns 3 + 4 = 7.
const char* kReturnLambdaTwoParams =
    "module M; class C "
    "  public static method Make(a: int, b: int): () -> int "
    "    return lambda() => a + b; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f: () -> int := C.Make(3, 4); "
    "    return f(); "   // 7
    "  end Main; "
    "end C;";

// (D) RETURN-VIA-VAR (the 9a2241e definite-escape form) with a parameter
// capture: `var f := lambda() => n; return f;`. The var is never reassigned, so
// the lambda it was initialized from definitely escapes; its sole capture cell —
// now a parameter's — is envdtor-freed. Returns 7.
const char* kReturnViaVarIntParam =
    "module M; class C "
    "  public static method Make(n: int): () -> int "
    "    var f: () -> int := lambda() => n; "
    "    return f; "
    "  end Make; "
    "  public static method Main(): int "
    "    var g: () -> int := C.Make(7); "
    "    return g(); "   // 7
    "  end Main; "
    "end C;";

// (E) THE ISSUE'S MEASUREMENT: 51 returned closures, each capturing a string
// parameter — pre-fix exactly 51 allocations / 408 bytes leaked (8 B per cell),
// the number recorded on #694. Post-fix: zero. Each closure is dropped at the
// end of its iteration, so all 51 envdtors run. Returns 51 (1 per iteration).
const char* kFiftyOneReturnedClosures =
    "module M; class C "
    "  public static method Make(s: string): () -> int "
    "    return lambda() => s.Length; "
    "  end Make; "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 51 do "
    "      var f: () -> int := C.Make(\"a\"); "
    "      total := total + f(); "
    "      i := i + 1; "
    "    end; "
    "    return total; "   // 51
    "  end Main; "
    "end C;";

// (F) SOUNDNESS CANARY — NON-escaping single capture of a parameter. The lambda
// is bound to a var and INVOKED in scope, never returned, so it is not in
// definitelyEscapingLambdas_ and the envdtor cell-free must NOT fire: the
// envdtor runs at scope end, where freeing the cell would UAF the outer scope's
// access through it. The scope-end boxedCellFree owns this cell instead (the
// param IS in cellFreeEligible_ — every capturing lambda is non-escaping), so
// this shape is LSan-clean both before and after the fix, and would flip to a
// double-free the moment the envdtor free were armed too. Returns 7.
const char* kNonEscapingParamCapture =
    "module M; class C "
    "  public static method Use(n: int): int "
    "    var f: () -> int := lambda() => n; "
    "    return f(); "
    "  end Use; "
    "  public static method Main(): int "
    "    return C.Use(7); "   // 7
    "  end Main; "
    "end C;";

// (G) SOUNDNESS CANARY — MULTI-HOLDER: parameter `n` is captured by TWO envs (a
// non-escaping lambda invoked in scope, and the returned one). captureCount is 2,
// so `n` is NOT in singleCaptureLocals_ and NEITHER env frees its cell — two
// envdtor frees of one cell would be a double-free. That cell leaks (bounded,
// the recorded safe direction; the general multi-holder case needs the
// refcounted-cell-header design noted on #694), so this uses the LENIENT checked
// run: a double-free or UAF still fails it. The local `bump`, captured by the
// single escaping env, IS freed — the same program pins both sides of the guard.
// Returns 7 + 0 + 8 = 15.
const char* kMultiHolderParamCapture =
    "module M; class C "
    "  public static method Make(n: int): (int) -> int "
    "    var g: () -> int := lambda() => n + 1; "
    "    var bump: int := g(); "        // 8 — proves g actually ran
    "    return lambda(k: int) => n + k + bump; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f: (int) -> int := C.Make(7); "
    "    return f(0); "   // 7 + 0 + 8 = 15
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #694 repro — a returned lambda capturing an `int` PARAMETER frees its
// capture cell. §28.9 (runtime-managed function values) / §27.5 (captures).
// =====================================================================
BEZEL_TEST(escaping_closure_param_cell_694,
           returned_lambda_int_param_cell_freed_lsan_clean_694) {
    expectCleanValue(kReturnLambdaIntParam, 7);
}

// =====================================================================
// (B) The pointer-sized (8 B) cell: a returned lambda capturing a `string`
// PARAMETER. §28.9 / §27.5.
// =====================================================================
BEZEL_TEST(escaping_closure_param_cell_694,
           returned_lambda_string_param_cell_freed_lsan_clean_694) {
    expectCleanValue(kReturnLambdaStringParam, 4);
}

// =====================================================================
// (C) Two parameters, two cells, one env — both freed. §28.9 / §27.5.
// =====================================================================
BEZEL_TEST(escaping_closure_param_cell_694,
           returned_lambda_two_params_cells_freed_lsan_clean_694) {
    expectCleanValue(kReturnLambdaTwoParams, 7);
}

// =====================================================================
// (D) The return-via-never-reassigned-var definite-escape form, with a
// parameter capture. §28.9 / §27.5.
// =====================================================================
BEZEL_TEST(escaping_closure_param_cell_694,
           returned_via_var_int_param_cell_freed_lsan_clean_694) {
    expectCleanValue(kReturnViaVarIntParam, 7);
}

// =====================================================================
// (E) The issue's recorded measurement — 51 returned closures, 51 capture
// cells, 408 bytes pre-fix, 0 after. §28.9 / §27.5.
// =====================================================================
BEZEL_TEST(escaping_closure_param_cell_694,
           fifty_one_returned_closures_param_cells_freed_lsan_clean_694) {
    expectCleanValue(kFiftyOneReturnedClosures, 51);
}

// =====================================================================
// (F) SOUNDNESS: a NON-escaping capture of a parameter is scope-end freed, not
// envdtor freed — no double-free, no UAF. §28.9 / §27.5 / §28.3.
// =====================================================================
BEZEL_TEST(escaping_closure_param_cell_694,
           non_escaping_param_capture_not_envdtor_freed_lsan_clean_694) {
    expectCleanValue(kNonEscapingParamCapture, 7);
}

// =====================================================================
// (G) SOUNDNESS: a parameter held by TWO envs is excluded from the envdtor
// free — a bounded leak, never a double-free. §28.9 / §27.5.
// =====================================================================
BEZEL_TEST(escaping_closure_param_cell_694,
           multi_holder_param_capture_no_double_free_694) {
    expectSafeValue(kMultiHolderParamCapture, 15);
}
