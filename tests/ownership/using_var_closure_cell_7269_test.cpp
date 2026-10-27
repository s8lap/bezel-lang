// Spec-derived regression test for issue #7269 — a `using var` local of
// FUNCTION TYPE leaked the closure cell, while the plain-`var` and `const`
// twins of the SAME program were clean.
//
// THE SPEC. §28.9 "Runtime-managed types" (v0.344), the function-typed
// bullet, verbatim:
//
//   "Function-typed fields, parameters, and locals are not subject to `owned`
//    annotations and carry no disposal obligation; a `using var` local of
//    function type is nonetheless permitted (disposal at scope end is a
//    no-op) and serves only the non-reassignability role (§28.10) — a plain
//    `var` of function type scopes its captures identically (§28.3's closure
//    bullet)."
//
// ⇒ THREE separate requirements, all pinned here:
//
//   (1) PERMITTED — "a `using var` local of function type is nonetheless
//       permitted". A fix that REJECTED this spelling would be
//       OVER-REJECTION against an explicit permission, and would break every
//       program that uses it. (Rows 7, 8.)
//   (2) THE NO-OP IS ABOUT THE OBLIGATION, NOT THE CELL — §28.9's "disposal
//       at scope end is a no-op" says the FUNCTION VALUE has no disposal
//       obligation (a function value is not a class, §8.7 — it has no
//       `Dispose()` to call). It is NOT a licence to skip releasing the
//       CLOSURE CELL, which is the heap object backing the value and which
//       the plain-`var` spelling releases. (Rows 1, 2, 3.)
//   (3) THE TWO SPELLINGS SCOPE THEIR CAPTURES IDENTICALLY — §28.9's last
//       clause. The capture cell of a `using var` closure must be reclaimed
//       exactly as the plain-`var` twin's is. (Row 2.)
//
// §28.10 "using var" (v0.344): "`using var` is a scope pin (axiom A5): it is
// implicitly non-reassignable, and reassigning a `using var` variable is a
// compile error." ⇒ the `using var` spelling of a function value must keep
// serving that role — a fix that dropped the pin would be as wrong as one
// that rejected the spelling. (Row 8.)
//
// THE DEFECT, MEASURED (parent commit, `bezelc --sanitize=address`, LSan):
//
//   using var c := lambda(): int => 42;   ->  64 byte(s) leaked in 1 alloc
//   var c := lambda(): int => 42;          ->  clean
//   const c := lambda(): int => 42;        ->  clean
//
// ⇒ THE THREE-SPELLING MATRIX, with the `using var` arm the odd one out —
// a direct contradiction of §28.9's own last clause. The capture-bearing
// twin leaked 76 byte(s) in 3 allocations (the 64-byte cell + the capture
// cells), where the `var` and `const` twins were clean.
//
// THE SITE. `Emitter::collectClosureReleaseEligible`
// (src/codegen/LlvmBackend_Setup.cpp) — its candidate walk excluded every
// `using var` local (`if (v.init && !v.isUsing)`), on the premise its own
// header stated: "Function-typed locals never appear in `using var` — §28.9".
// §28.9 says the opposite ("a `using var` local of function type is
// nonetheless permitted"), so the comment was a FALSE PREMISE and the
// exclusion it drove was a leak. The producer's +1 on the closure cell was
// never released.
//
// ── THE ORACLES (and why they differ per row) ─────────────────────────────
//
// ⚠ LEAK ROWS USE THE CHECKED ORACLE. `runAot` pins `detect_leaks=0`
// (tests/support/aot_support.hpp), so a plain `runAot` row is VACUOUS for a
// leak — the program can leak and the row stays green. `runAotChecked` sets
// `detect_leaks=1` and reports a leak as a sanitizer failure, so
// `r.ran == false` is the leak verdict. The toolchain skip is asserted
// LOUDLY (`checkedToolchainSkipped`), never absorbed by a `return` — a box
// whose clang cannot link ASan has not tested the memory claim, and a silent
// skip is indistinguishable from a pass.
//
// ⚠ THE DIAGNOSTIC ROWS CANNOT USE `runAot`. `runAot` DISCARDS the
// compiler's stderr on a successful compile, so a row that inspected
// `r.diagnostics` for a warning would see NOTHING on either arm — VACUOUS.
// ⇒ The §28.10 "vacuous" warning is pinned through the OWNERSHIP PASS run
// IN-PROCESS over the test's OWN `DiagnosticEngine`
// (`bezel::test::parseAndCheckOwnership`, tests/support/ownership_support.hpp)
// — the pass that actually emits the lint (src/ownership/Ownership.cpp).
// `DiagnosticEngine::report` records EVERY severity, so the warning lands in
// the test's engine and the row can assert its presence AND its absence.
//
// ROWS (each named below):
//
//   1. LEAK          `using var` + lambda literal        -> no leak (RED before)
//   2. LEAK          `using var` + capture-bearing twin  -> no leak (RED before)
//   3. LEAK          `using var` + method-return producer-> no leak (RED before)
//   4. CONTROL       plain-`var` twin                    -> no leak (green both)
//   5. CONTROL       `const` twin                        -> no leak (green both)
//   6. CONTROL       borrow-side `using var` (field read)-> no leak (green both)
//   7. PERMISSION    `using var` of a function value is PERMITTED and callable
//   8. §28.10        the `using var` pin still rejects reassignment
//   9. DIAGNOSTIC    the §28.10 "vacuous" warning STILL fires for the
//                    `using var` spelling (green on both arms)
//  10. DIAGNOSTIC    and it does NOT fire for the plain-`var` twin
//  11. CONTROL       an OWNED `using var` (a class-typed binding with a real
//                    disposal obligation) still disposes at scope end — LSan
//                    clean is the assertion that the disposal still fired

#include <cstdio>
#include <string>

#include "support/Diagnostic.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ── the §28.10 "vacuous" warning needle ────────────────────────────────────
// The exact wording of the ownership pass's lint, stable fragments only:
// "`using var` `c` is vacuous" + "no disposal obligation (§28.9)".
const char* kVacuous = "is vacuous";
const char* kVacuousObligation = "no disposal obligation";

// ── the §28.10 non-reassignability needle ──────────────────────────────────
const char* kReassign = "cannot reassign `using var` local";

// ── the programs ───────────────────────────────────────────────────────────

// (1) THE ISSUE'S REPRO. §28.9's "a `using var` local of function type is
// nonetheless permitted" — the spelling must be admitted, and the closure
// cell it binds must be released at scope end.
const char* kUsingVarLambda =
    "module M7269; "
    "public class C "
    "  public static method Main(): int "
    "    using var c := lambda(): int => 42; "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (2) THE CAPTURE-BEARING TWIN. §28.9's last clause ("a plain `var` of
// function type scopes its captures identically") — the CAPTURE CELL must be
// reclaimed too, not only the 64-byte closure cell.
const char* kUsingVarCapture =
    "module M7269b; "
    "public class C "
    "  public static method Main(): int "
    "    var k: int := 7; "
    "    using var c := lambda(): int => k; "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (3) THE #429 RETURN-CATEGORY PRODUCER. The callee mints exactly +1 on
// return; the caller's local transfers that single reference and must release
// it at scope end iff non-escaping — for the `using var` spelling too.
const char* kUsingVarMethodReturn =
    "module M7269c; "
    "public class C "
    "  public static method MakeFn(): (int) -> int "
    "    return lambda(x: int): int => x + 1; "
    "  end MakeFn; "
    "  public static method Main(): int "
    "    using var g := MakeFn(); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (4) CONTROL — the plain-`var` twin of (1). Green on BOTH arms: it is the
// reference behaviour the fix must reproduce, and it is the row that proves
// the fix did not widen the candidate walk's reach.
const char* kVarLambda =
    "module M7269v; "
    "public class C "
    "  public static method Main(): int "
    "    var c := lambda(): int => 42; "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (5) CONTROL — the `const` twin of (1). Green on BOTH arms (#3402 already
// admits the `const` node into the same candidate walk).
const char* kConstLambda =
    "module M7269k; "
    "public class C "
    "  public static method Main(): int "
    "    const c := lambda(): int => 42; "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (6) CONTROL — the BORROW side of the same walk: a function-typed local
// bound to a FIELD read. The slot owns nothing at bind (the +1 belongs to
// the source), so the walk's borrow arm does not admit the `using var`
// spelling — and both spellings are LSan-clean. This row proves the fix did
// not widen the borrow arm.
const char* kUsingVarFieldRead =
    "module M7269f; "
    "public class C "
    "  public F: (int) -> int; "
    "  public constructor() "
    "    F := lambda(x: int): int => x + 1; "
    "  end constructor; "
    "  public static method Main(): int "
    "    var h := new C(); "
    "    using var g := h.F; "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (7) PERMISSION + the value channel: `using var` of a function value is
// PERMITTED (§28.9's "nonetheless permitted") and the closure is still
// callable in scope — a fix that rejected the spelling, or that released the
// cell too early, would break this row.
const char* kUsingVarCalled =
    "module M7269p; "
    "public class C "
    "  public static method Main(): int "
    "    using var c := lambda(): int => 42; "
    "    return c(); "
    "  end Main; "
    "end C; ";

// (8) §28.10's non-reassignability role: the `using var` spelling of a
// function value must STILL be a scope pin.
const char* kUsingVarReassigned =
    "module M7269r; "
    "public class C "
    "  public static method Main(): int "
    "    using var c := lambda(): int => 42; "
    "    c := lambda(): int => 43; "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (11) CONTROL — an OWNED `using var` (a class-typed binding, which DOES
// carry a disposal obligation): its scope-end disposal must still fire.
const char* kOwnedUsingVar =
    "module M7269o; "
    "public class C "
    "  public static method Main(): int "
    "    using var a: Array<int> := new Array<int>(); "
    "    a.Add(7); "
    "    Console.WriteLine(a.Count); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// ── the leak oracle ────────────────────────────────────────────────────────
// Assert: the program compiled, linked, ran to completion UNDER ASan/LSan
// with NO leak, and exited with `expectedExit`.
//
// `runAotChecked` is the CHECKED oracle: it sets `detect_leaks=1`, so a leak
// is a sanitizer failure and `r.ran` stays false. `runAot` would be
// VACUOUS here — it pins `detect_leaks=0`.
//
// The toolchain skip is asserted, not absorbed: a `return` on a SKIP would
// make an untested memory claim indistinguishable from a pass.
void expectNoLeak(const char* source, int expectedExit) {
    JitResult r = runAotChecked(source);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "DIAG [%d]: %s\n", (int)d.severity,
                         d.message.c_str());
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, expectedExit);
}

// ── the diagnostic oracle (the ownership pass, in-process) ─────────────────
// `runAot` DISCARDS a successful compile's stderr, so the §28.10 warning is
// invisible to it. This drives the OWNERSHIP PASS (the pass that emits the
// lint) over the test's OWN DiagnosticEngine, which records EVERY severity.
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

int countWarningSubstring(const OwnershipResult& r, const char* needle) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find(needle) != std::string::npos) ++n;
    }
    return n;
}

void dump(const OwnershipResult& r) {
    std::fprintf(stderr, "  hasErrors=%d diagnostics (%zu):\n",
                 (int)r.diagnostics.hasErrors(),
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%d] %s\n", (int)d.severity,
                     d.message.c_str());
}

}  // namespace

// ===========================================================================
// ROW 1 — LEAK: the `using var` spelling of a function-typed local must
// release the closure cell at scope end. RED before the fix: the base arm
// leaked 64 byte(s) in 1 allocation, so `runAotChecked` reported a sanitizer
// failure and `r.ran` was false.
// ===========================================================================
BEZEL_TEST(ownership_using_var_closure_cell_7269,
           using_var_closure_local_releases_its_cell) {
    expectNoLeak(kUsingVarLambda, 0);
}

// ===========================================================================
// ROW 2 — LEAK: the CAPTURE-BEARING twin. §28.9's last clause — "a plain
// `var` of function type scopes its captures identically" — so the capture
// cell must be reclaimed as well as the 64-byte closure cell. RED before the
// fix: 76 byte(s) leaked in 3 allocations.
// ===========================================================================
BEZEL_TEST(ownership_using_var_closure_cell_7269,
           using_var_closure_local_with_capture_releases_cell_and_capture) {
    expectNoLeak(kUsingVarCapture, 0);
}

// ===========================================================================
// ROW 3 — LEAK: the #429 RETURN-category producer. The callee mints exactly
// +1 on return and the caller's local transfers that reference, so the
// `using var` spelling must release it too. RED before the fix: 64 byte(s)
// leaked in 1 allocation.
// ===========================================================================
BEZEL_TEST(ownership_using_var_closure_cell_7269,
           using_var_closure_local_from_method_return_releases_its_cell) {
    expectNoLeak(kUsingVarMethodReturn, 0);
}

// ===========================================================================
// ROW 4 — CONTROL: the plain-`var` twin. GREEN ON BOTH ARMS — it is the
// reference behaviour, and it proves the fix did not widen the candidate
// walk's reach beyond the `using var` spelling.
// ===========================================================================
BEZEL_TEST(ownership_using_var_closure_cell_7269,
           var_closure_local_still_releases_its_cell) {
    expectNoLeak(kVarLambda, 0);
}

// ===========================================================================
// ROW 5 — CONTROL: the `const` twin. GREEN ON BOTH ARMS (#3402 admits the
// `const` node into the same candidate walk; the fix must not disturb it).
// ===========================================================================
BEZEL_TEST(ownership_using_var_closure_cell_7269,
           const_closure_local_still_releases_its_cell) {
    expectNoLeak(kConstLambda, 0);
}

// ===========================================================================
// ROW 6 — CONTROL: the BORROW side of the same walk (a function-typed local
// bound to a field read). The slot owns nothing at bind, so the walk's
// borrow arm does not admit the `using var` spelling — and both spellings are
// LSan-clean. GREEN ON BOTH ARMS: the fix must not have widened the borrow
// arm.
// ===========================================================================
BEZEL_TEST(ownership_using_var_closure_cell_7269,
           using_var_closure_local_from_field_read_is_clean) {
    expectNoLeak(kUsingVarFieldRead, 0);
}

// ===========================================================================
// ROW 7 — PERMISSION (the over-rejection guard). §28.9: "a `using var` local
// of function type is nonetheless permitted". The spelling must be ADMITTED
// and the closure must still be callable in scope. A fix that rejected the
// spelling, or that released the cell before the call, breaks this row.
//
// GREEN ON BOTH ARMS (the spelling was always permitted; only the cell
// release was missing).
// ===========================================================================
BEZEL_TEST(ownership_using_var_closure_cell_7269,
           using_var_closure_local_is_permitted_and_callable) {
    JitResult r = runAot(kUsingVarCalled);
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "DIAG [%d]: %s\n", (int)d.severity,
                         d.message.c_str());
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// ROW 8 — §28.10's non-reassignability role. The `using var` spelling of a
// function value must STILL be a scope pin: reassigning it is a compile
// error. A fix that dropped the pin would be as wrong as one that rejected
// the spelling.
//
// GREEN ON BOTH ARMS.
// ===========================================================================
BEZEL_TEST(ownership_using_var_closure_cell_7269,
           using_var_closure_local_is_still_not_reassignable) {
    JitResult r = runAot(kUsingVarReassigned);
    BEZEL_EXPECT_FALSE(r.ran);
    // The rejection must be a REAL compile-time diagnostic with a real
    // source location — not a crash, a timeout or a sanitizer report.
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, kReassign));
}

// ===========================================================================
// ROW 9 — DIAGNOSTIC: the §28.10 lint ("`using var` `c` is vacuous — the
// RHS is a value-typed/runtime-managed value with no disposal obligation
// (§28.9)") is CORRECT about the obligation and must STAY: the fix changes
// the CELL's lifetime, not the value's (absent) disposal obligation.
//
// GREEN ON BOTH ARMS. Observed through the IN-PROCESS front-end + codegen
// oracle, because `runAot` discards a successful compile's stderr and would
// make this row VACUOUS.
// ===========================================================================
BEZEL_TEST(ownership_using_var_closure_cell_7269,
           using_var_closure_local_still_warns_vacuous) {
    OwnershipResult r = parseAndCheckOwnership(kUsingVarLambda);
    if (r.diagnostics.hasErrors()) dump(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(countWarningSubstring(r, kVacuous), 1);
    BEZEL_EXPECT_EQ(countWarningSubstring(r, kVacuousObligation), 1);
}

// ===========================================================================
// ROW 10 — DIAGNOSTIC: the lint is specific to the `using var` spelling —
// the plain-`var` twin must NOT warn. GREEN ON BOTH ARMS.
// ===========================================================================
BEZEL_TEST(ownership_using_var_closure_cell_7269,
           var_closure_local_does_not_warn_vacuous) {
    OwnershipResult r = parseAndCheckOwnership(kVarLambda);
    if (r.diagnostics.hasErrors()) dump(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(countWarningSubstring(r, kVacuous), 0);
}

// ===========================================================================
// ROW 11 — CONTROL: an OWNED `using var` (a class-typed binding, which DOES
// carry a disposal obligation) must still dispose at scope end. LSan-clean IS
// the assertion that the disposal fired — a fix that dropped it would leak
// the array and this row would go red.
//
// GREEN ON BOTH ARMS.
// ===========================================================================
BEZEL_TEST(ownership_using_var_closure_cell_7269,
           owned_using_var_still_disposes_at_scope_end) {
    JitResult r = runAot(kOwnedUsingVar);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
    // And under the leak oracle: no leak ⇒ the scope-end disposal fired.
    expectNoLeak(kOwnedUsingVar, 0);
}
