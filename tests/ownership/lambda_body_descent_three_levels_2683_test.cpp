// Spec-derived regression tests for issue #2683 — `sema` descends into
// lambda bodies (per `ExprTypecheck::visitStmt`'s
// `for (const auto& s : lam->stmts) if (s) visitStmt(*s);` shape in
// ExprTypecheck_Exprs.cpp ~line 8066), but the ownership pass's
// `dataflow`-driven `visitStmt` walk NEVER does (the walker is keyed on
// the per-method CFG, which by construction stops at a `LambdaExpr`'s
// body — see the Stage 1 #4146 commit body on `b75d910d`). Instead the
// ownership pass runs the narrow purpose-built
// `applyUsingVarBorrowPinInLambdas` pre-pass, which mirrors
// `collectBodyLambdas`'s shape — it descends into EVERY nesting level,
// including nested lambdas inside another lambda body, because
// `collectBodyLambdas` itself recurses.
//
// #2683 was filed because that pre-pass had been CLAIMED to descend but
// had never been TESTED at the depth the campaign §2.4 names. The
// existing #4033 row (lambda_owned_return_direct_borrow_4005_test.cpp
// Row "nested_lambda_body_local_decl_target_rejected_spec_28_3") only
// pins TWO levels of nesting — a regression that broke the descent past
// ONE level would already fail it. The deeper pin at THREE levels (this
// file) is what campaign §2.4 actually asks for: the pre-pass must walk
// the middle lambda's own body to REGISTER the inner lambda, and the
// middle lambda's body to fire the OWNED-RETURN-BORROW classification on
// it. A regression that broke descent at the second-or-deeper boundary
// would fail this file while passing every prior regression.
//
// ROW PLAN
//   Row 1: three-level nested lambda body returning a captured `using
//          var b` through a `() -> owned Res` slot at the innermost
//          depth. The OWNED-RETURN-BORROW classification must reach the
//          innermost lambda. Without the descent, the program compiles
//          clean (silently) and double-frees `b` at runtime; the
//          compile-time rejection here pins the classification reach.
//          This is the `kLambdaOwnedBorrowReturn` rejection
//          (#4005/#4033/#4147 family).
//   Row 2 (accept-side control, §15): three-level nested lambda body
//          whose INNERMOST lambda returns a fresh production (no
//          capture) through the same `() -> owned Res` slot. Pinned to
//          prove the descent doesn't introduce false rejects at depth
//          (every level produces fresh values; classification accepts
//          all of them).
//   Row 3: three-level nested lambda body holding a `LocalConstDecl`
//          (the sibling case the LocalDecl-only walker ratchet covers
//          — see scripts/check_localdecl_only_walkers.py and #3591).
//          The pre-pass must walk the `LocalConstDecl` at every level
//          too.
//
// §28.2 (Ownership discipline), §28.3 (Borrow lifetime rules, concrete-
// slot function bodies), §28.10 (Using-var borrow pin), v0.298.
//
// per §28.2, §28.3, §28.10, v0.298, #2683, #4033

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation,
                                            // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/Diagnostic.hpp"
#include "support/ownership_support.hpp"   // parseAndCheckOwnership
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// #4005 / #4033 / #4147 family diagnostic — distinct wording from the
// named-method form (which this file does not reach; a lambda body is a
// structurally different code path).
constexpr const char* kLambdaOwnedBorrowReturn =
    "the lambda is checked against a target function type whose return is "
    "`owned`";

// A destructor-counter Res: ctor ++, dtor --, surfaced through Res.live so
// an ACCEPT test can assert exact-once disposal (0 == no leak, no
// double-free).
const char* kRes =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// REJECT — three-level nested lambda body returning a captured `using var b`
// through a `() -> owned Res` slot at the innermost depth. The OWNED-
// RETURN-BORROW classification must REACH the innermost lambda body. The
// pre-pass walks the outer lambda's body, finds the middle lambda's body,
// walks THAT (which is the depth the existing #4033 row stops at), and
// then walks the middle lambda's body AGAIN to find the inner lambda — a
// third level of descent that this row pins. Without it the program
// compiles clean and double-frees `b` at runtime (the silent un-reject
// #2683 names); with it the program is rejected at compile time with
// `kLambdaOwnedBorrowReturn`.
//
// Note: the lambda LITERAL itself must be `lambda() => b`; the
// `() -> owned Res` is supplied by the typed `var mk` slot's declared
// type. A literal annotated `(): owned Res => b` is the RETIRED pre-v0.217
// lambda spelling (§27.3, v0.217).
// ===========================================================================
BEZEL_TEST(lambda_body_descent_three_levels_2683,
           nested_three_levels_using_var_captured_owned_slot_rejected_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var outer := lambda(): int => do "
        "      var mid := lambda(): int => do "
        "        var mk: () -> owned Res := lambda() => b; "
        "        var c := mk(); "
        "        return c.tag; "
        "      end; "
        "      return mid(); "
        "    end; "
        "    var t := outer(); return t; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// ===========================================================================
// ACCEPT — three-level nested lambda body where the INNERMOST lambda
// returns a fresh `new Res(...)` production (no captured borrow) through
// the same `() -> owned Res` slot. Pinned to prove the descent does not
// introduce false rejects at depth — every level produces a fresh value
// the classification accepts.
// ===========================================================================
BEZEL_TEST(lambda_body_descent_three_levels_2683,
           nested_three_levels_fresh_owned_producer_accepted_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var outer := lambda(): int => do "
        "      var mid := lambda(): int => do "
        "        var mk: () -> owned Res := lambda() => new Res(7); "
        "        do using var c := mk(); end; "
        "        return 0; "
        "      end; "
        "      return mid(); "
        "    end; "
        "    var t := outer(); return t; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
}

// ===========================================================================
// REJECT — three-level nested lambda body holding a `LocalConstDecl` (the
// sibling case the LocalDecl-only walker ratchet covers — see
// scripts/check_localdecl_only_walkers.py and #3591). The pre-pass must
// walk the `LocalConstDecl` at every level; a regression that skipped
// `LocalConstDecl` would silently accept a borrowed-returning const
// initializer at the innermost depth.
// ===========================================================================
BEZEL_TEST(lambda_body_descent_three_levels_2683,
           nested_three_levels_local_const_decl_using_var_captured_rejected_21_1) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var outer := lambda(): int => do "
        "      var mid := lambda(): int => do "
        "        const mk: () -> owned Res := lambda() => b; "
        "        var c := mk(); "
        "        return c.tag; "
        "      end; "
        "      return mid(); "
        "    end; "
        "    var t := outer(); return t; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}
