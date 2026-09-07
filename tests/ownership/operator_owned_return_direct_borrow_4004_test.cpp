// Spec-derived regression tests for issue #4004 — an `operator` declaration's
// body NEVER reached `handleReturn`'s return-type-aware checks at all (not
// just the #3993 one), because `checkMember`'s OperatorDecl arm called
// `mo.run(od->stmts)` (block body) with every optional argument at its
// default (paramSyms=nullptr, returnType=nullptr, returnOwned=false,
// isStatic=false), and drove the EXPRESSION-bodied arm through nothing but
// the narrow SB01-B lambda-borrow-return pre-pass. The #3993 fix
// (`checkOwnedReturnDirectBorrow`) is unreachable through either path, so
// the IDENTICAL borrow-through-owned-return double-free #3993 closed for a
// named method reproduces UNCHANGED through `operator +`.
//
// Spec, v0.294, §28.3 "Borrow lifetime rules" (verbatim, first bullet):
//   "A borrowed local may not be returned from a method whose return type
//    is `owned`."
// §25 establishes an operator is declared `public static method operator
// ...` — an ordinary static method for every other ownership purpose the
// pass cares about, so the driver gap here is purely a wiring omission, not
// a deliberate operator/method distinction.
//
// THE BUG (measured on this worktree with #3993's fix — `handleReturn`'s
// own `checkOwnedReturnDirectBorrow` — ALREADY APPLIED, i.e. the middle
// state of a three-way fail-without):
//
//   public static method operator +(a: Res, b: Res): owned Res
//     return b;
//   end operator +;
//
//   compiled with rc=0 and zero diagnostics (only the unrelated, expected
//   "`c` is implicitly disposed at scope end" auto-drop note); running the
//   compiled program under AddressSanitizer produced a heap-use-after-free —
//   `b`'s object freed once via `c`'s scope-end auto-drop (its tracked state
//   is Owned, taken from the DECLARED `owned` operator return type) and
//   again via `b`'s own `using var` scope-end disposal. The `=> b;`
//   expression-bodied form reproduces identically.
//
// THE FIX: wire the operator's own signature into `mo.run(...)` (block
// body) / `mo.runExprBodyOwningReturnCheck(...)` (expression body) exactly
// as the sibling MethodDecl arm already does — `paramSymsOf(m, ctx)`,
// `od->returnType.get()`, `od->returnOwned`, `isStatic=true` (§25: an
// operator is ALWAYS static), plus a GenericReturnContext populated from the
// enclosing type only (an operator carries no method-level generic
// parameters of its own, per A.4 / checkOperatorReturnDeclaration's own
// comment). This activates EVERY dormant return-type-aware check for
// EVERY existing operator — not just #3993's — including
// `checkStaticBorrowReturn` (a plain, non-`owned`-returning operator
// returning a bare borrowed parameter, the operator-shaped twin of the
// spec's own `static method Pick(a: Res, b: Res): Res return a;` example).
// A broad `operator`-filtered sweep (399 tests across 6 binaries, 0 failed)
// found no false-reject fallout from activating this in the existing
// corpus.
//
// per §28.3 *Borrow lifetime rules*, §25 *Operator overloading*, v0.294

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot / runAotChecked — AOT(+ASan) oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The #3993 direct-borrow-return diagnostic's stable substring — this fix
// does not introduce a new message, it makes the #3993 one REACHABLE for an
// operator body.
constexpr const char* kDirectBorrowReturn =
    "a borrow cannot be returned as an `owned` value";

// The pre-existing checkStaticBorrowReturn diagnostic's stable substring —
// newly REACHABLE for an operator body once this fix wires `isStatic=true`.
constexpr const char* kStaticBorrowReturn =
    "a borrow may not be returned from a static method";

// A destructor-counter Res, with `operator +` declared AS A MEMBER (§25.2:
// "at least one parameter of operator `+` must be the declaring type" — the
// operator must live on `Res` itself, not on an unrelated caller class), its
// body supplied by the caller so each test can vary just the return
// statement / expression. `memberBody` is the operator's FULL member
// declaration text (`public static method operator + ... end operator +;`).
std::string resWithOperator(const std::string& memberBody) {
    return
        "module M; "
        "class Res "
        "  public static live: int := 0; "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; end constructor; "
        "  destructor Res.live := Res.live - 1; end destructor; " +
        memberBody +
        "end Res; ";
}

}  // namespace

// ===========================================================================
// REJECT — a direct borrowed-parameter return through an `owned` operator
// return type. Fail-without (both #3993 alone and neither fix): accepted
// with zero diagnostics, double-freed at runtime (ASan heap-use-after-free,
// measured). Pass-with (this fix): rejected at compile time with a real
// source location.
// ===========================================================================

// The issue's exact repro, BLOCK-bodied operator (declared on `Res` — §25.2).
BEZEL_TEST(operator_owned_return_direct_borrow_4004,
           block_bodied_operator_borrowed_param_rejected_spec_28_3) {
    JitResult jr = runAot(
        resWithOperator(
            "  public static method operator +(a: Res, b: Res): owned Res "
            "    return b; "
            "  end operator +; ") +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Res(3); using var b := new Res(4); "
        "    var c := a + b; return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// The `=>` EXPRESSION-bodied sibling of the same shape.
BEZEL_TEST(operator_owned_return_direct_borrow_4004,
           expr_bodied_operator_borrowed_param_rejected_spec_28_3) {
    JitResult jr = runAot(
        resWithOperator(
            "  public static method operator +(a: Res, b: Res): owned Res => b; ") +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Res(3); using var b := new Res(4); "
        "    var c := a + b; return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// ===========================================================================
// REJECT — the OTHER dormant check this fix newly activates for operators:
// a PLAIN (non-`owned`) static operator returning a bare borrowed parameter
// has no receiver to source the borrow from, exactly like the spec's own
// `static method Pick(a: Res, b: Res): Res return a;` example (§28.3). This
// is real, deliberate NEW behavior from wiring `isStatic=true` — pin it so a
// future change cannot silently re-disable it.
// ===========================================================================

BEZEL_TEST(operator_owned_return_direct_borrow_4004,
           plain_return_type_static_borrow_return_now_rejected_spec_28_3) {
    JitResult jr = runAot(resWithOperator(
        "  public static method operator +(a: Res, b: Res): Res "
        "    return a; "
        "  end operator +; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kStaticBorrowReturn));
}

// ===========================================================================
// ACCEPT — a genuinely owned production through an operator is NOT
// over-rejected, and disposes the value EXACTLY once under the AOT+ASan
// oracle (Res.live == 0 at exit — no leak, no double-free).
// ===========================================================================

// A fresh `new` production — the ordinary factory shape this rule must not
// disturb, block-bodied.
BEZEL_TEST(operator_owned_return_direct_borrow_4004,
           block_bodied_fresh_new_accepted_disposes_once_spec_28_3) {
    std::string src =
        resWithOperator(
            "  public static method operator +(a: Res, b: Res): owned Res "
            "    return new Res(a.tag + b.tag); "
            "  end operator +; ") +
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var a := new Res(3); using var b := new Res(4); "
        "      do using var c := a + b; end; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// The same, expression-bodied.
BEZEL_TEST(operator_owned_return_direct_borrow_4004,
           expr_bodied_fresh_new_accepted_disposes_once_spec_28_3) {
    std::string src =
        resWithOperator(
            "  public static method operator +(a: Res, b: Res): owned Res => "
            "    new Res(a.tag + b.tag); ") +
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var a := new Res(3); using var b := new Res(4); "
        "      do using var c := a + b; end; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// A relayed OWNED call result — the callee-returns-`owned`-T producer form,
// through an operator.
BEZEL_TEST(operator_owned_return_direct_borrow_4004,
           relayed_owned_call_result_accepted_disposes_once_spec_28_3) {
    std::string src =
        resWithOperator(
            "  public static method Make(a: Res, b: Res): owned Res "
            "    return new Res(a.tag + b.tag); "
            "  end Make; "
            "  public static method operator +(a: Res, b: Res): owned Res "
            "    return Make(a, b); "
            "  end operator +; ") +
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var a := new Res(3); using var b := new Res(4); "
        "      do using var c := a + b; end; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// A plain (non-`owned`) operator returning `this`-independent borrowed data
// via a receiverless-but-legal path must stay unaffected: returning a VALUE
// (non-borrowable) type is never flagged by checkStaticBorrowReturn.
BEZEL_TEST(operator_owned_return_direct_borrow_4004,
           value_typed_return_not_flagged_by_static_borrow_check) {
    JitResult jr = runAot(
        "module M; "
        "class C "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  public static method operator +(a: C, b: C): int return a.tag + b.tag; end operator +; "
        "  public static method Main(): int "
        "    var a := new C(3); var b := new C(4); return a + b; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 7);
}
