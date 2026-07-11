// Spec-derived tests for #1688 — §17.14 (v0.184) constant-expression
// evaluation faults are compile errors, for a LOCAL `const` declaration.
//
// Authority — spec §17.14 "Evaluation" (v0.184):
//   "An evaluation **fault** — integer or `decimal` overflow, division or
//   `mod` by zero — is a **compile error at the constant expression**,
//   never a deferred runtime fault: `const B := 2147483647 + 1;` and
//   `const D := 5 / 0;` are rejected where they are written, the same
//   posture as §5.1's constant-narrowing range error."
//
// §17.14 draws no distinction between a class-level `const` member and a
// local `const` (grammar A.6 `LocalConstDecl`) — both are the same
// constant-expression sublanguage, and the section's own example
// (`const B := 2147483647 + 1;`) is written with no enclosing-context cue.
//
// Bug: TypeResolution_Checks.cpp's checkTypeConsts already enforced this for
// a CLASS-level const member (#958 overflow, #1211 div/mod-by-zero) — see
// tests/sema/const_type_test.cpp. But ExprTypecheck.cpp's LocalConstDecl
// statement handler (the pass that type-checks a *local* const inside a
// method body) never called the equivalent fold-and-check logic at all: a
// local `const B := 2147483647 + 1;` or `const D := 5 / 0;` compiled with NO
// diagnostic and deferred the fault to a runtime OverflowException /
// DivideByZeroException — confirmed by direct bezelc repro before this fix.
//
// Fix: LocalConstDecl's ExprTypecheck.cpp handler now folds the initializer
// (foldIntConstExprTC) against the declared-or-inferred integer target type
// and reports a §17.14 compile error on overflow or division/modulo by zero
// — mirroring checkTypeConsts via two now-shared ConstFold.hpp helpers
// (fitsConstIntRange, containsConstDivByZero) so both const forms agree on
// exactly one definition of "fits" / "div-by-zero".

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// Runs BOTH sema passes: resolveTypes (type resolution) then
// typecheckExpressions (the ExprTC pass whose LocalConstDecl handler this
// fix touches) — mirrors tests/sema/const_assignment_747_test.cpp. A
// resolveTypes-only harness (as in const_type_test.cpp, which only exercises
// the CLASS-level const path) would never reach the local-const check.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasMessageContaining(const bezel::test::SemaResult& r,
                           std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

std::string mainWith(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

}  // namespace

// ── §17.14: local const overflow is a compile error (fails without the fix) ──

// §17.14: `const B := 2147483647 + 1;` — the spec's own example, at local
// scope. Pre-fix: compiled clean, raised OverflowException at runtime.
BEZEL_TEST(local_const_eval_fault_1688, int_overflow_rejected) {
    auto r = parseSemaAll(mainWith("const B := 2147483647 + 1;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMessageContaining(r, "does not fit its declared type"));
    BEZEL_EXPECT_TRUE(hasMessageContaining(r, "§17.14"));
}

// §17.14: an explicitly-typed narrower local const still range-checks the
// folded value against the ANNOTATED type, not just an inferred `int`.
BEZEL_TEST(local_const_eval_fault_1688, explicit_type_overflow_rejected) {
    auto r = parseSemaAll(mainWith("const B: byte := 300;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMessageContaining(r, "does not fit its declared type"));
    BEZEL_EXPECT_TRUE(hasMessageContaining(r, "§17.14"));
}

// §17.14: `const D := 5 / 0;` — the spec's own example, at local scope.
// Pre-fix: compiled clean, raised DivideByZeroException at runtime.
BEZEL_TEST(local_const_eval_fault_1688, div_by_zero_rejected) {
    auto r = parseSemaAll(mainWith("const D := 5 / 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasMessageContaining(r, "division or modulo by zero"));
    BEZEL_EXPECT_TRUE(hasMessageContaining(r, "§17.14"));
}

// §17.14: `mod 0` is equally a division-by-zero evaluation fault.
BEZEL_TEST(local_const_eval_fault_1688, mod_by_zero_rejected) {
    auto r = parseSemaAll(mainWith("const D := 5 mod 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasMessageContaining(r, "division or modulo by zero"));
    BEZEL_EXPECT_TRUE(hasMessageContaining(r, "§17.14"));
}

// ── Positive controls: valid constants still fold correctly ──────────────────

// §17.14 example: `const H := int.MaxValue / 2;` style non-overflowing
// division (using a literal in place of int.MaxValue, which is a separate,
// unrelated admissibility gap — #1688 item 2) must still compile clean.
BEZEL_TEST(local_const_eval_fault_1688, valid_fold_accepted) {
    auto r = parseSemaAll(mainWith("const H := 2147483647 / 2;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A local const referencing an earlier local const (const-of-const) still
// folds and range-checks correctly — the fix must not blunt genuine folding.
BEZEL_TEST(local_const_eval_fault_1688, const_of_const_accepted) {
    auto r = parseSemaAll(mainWith("const A := 1000 + 1; const B := A * 2;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A non-zero-divisor `/` and `mod` still fold without error (regression
// guard on the fold path itself, mirroring const_type_test.cpp's class-level
// equivalents).
BEZEL_TEST(local_const_eval_fault_1688, nonzero_div_and_mod_accepted) {
    auto r = parseSemaAll(mainWith("const A := 10 / 2; const B := 7 mod 3;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── Positive control: a RUNTIME (non-constant) fault is NOT rejected here ────

// §17.14 only governs a compile-time-constant expression. A `/` between two
// ordinary (non-const) `var` locals is a runtime expression — division by
// zero there is (and must remain) a runtime DivideByZeroException, not a
// compile error. This pins that the fix's fold-and-check gate is scoped to
// `const` initializers and does not reach into ordinary expressions.
BEZEL_TEST(local_const_eval_fault_1688,
           runtime_division_by_zero_not_a_compile_error) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var x := 5; var y := 0; var z := x / y; return z; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
