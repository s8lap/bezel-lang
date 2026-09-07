// Spec-derived regression tests for issue #4034 — the last disclosed-open gap
// in the #4005 family: a lambda literal passed DIRECTLY AS A CALL ARGUMENT to
// a `() -> owned T`-typed parameter was never checked for returning a
// borrow, because the two existing "what is this lambda's expected target
// type" collectors — `collectReturnDirectLambdas` (a direct `return`) and
// `collectLocalDeclDirectLambdaTargets` (#4005, a typed LocalDecl
// initializer) — can only see a target position that IS a ReturnStmt or a
// LocalDecl. An argument position has neither; the target type comes from
// the CALLEE's own formal parameter list, which #4005's review flagged as
// needing "callee-signature threading, a materially different mechanism"
// and deliberately left open.
//
// §28.3 *Borrow lifetime rules*, v0.294 (verbatim, first bullet): "A
// borrowed local may not be returned from a method whose return type is
// `owned`." §28.3 *Concrete-slot function bodies* extends this to any
// function-typed slot, including one supplied as a call argument.
//
// THE BUG (measured on this worktree, based on the r15-c3 batch carrying
// #4004/#4005/#4033/#4035/#4043 — i.e. every OTHER position in this family
// already fixed):
//
//   class C
//     public static method Consume(mk: () -> owned Res): int
//       var c := mk();
//       return c.tag;
//     end Consume;
//     public static method Main(): int
//       using var b := new Res(4);
//       return Consume(lambda() => b);
//     end Main;
//   end C;
//
//   compiled with rc=0 and zero diagnostics; running the compiled binary
//   produced `Segmentation fault (core dumped)` (the issue's own measured
//   repro) — the same class of heap corruption as #3993/#4005: the lambda's
//   captured `using var b` is disposed once via `c`'s scope-end auto-drop
//   (tracked Owned off `Consume`'s declared `owned` parameter) and again via
//   `b`'s own `using var` scope-end disposal.
//
// THE FIX: a new collector pair,
// `collectCallArgDirectLambdaTargets`/`-Expr` (Ownership_Checks.cpp,
// declared in Ownership_impl.hpp), mirroring `collectBodyLambdas`/
// `collectBodyLambdasExpr`'s FULL statement+expression traversal (a call
// site can appear at any expression nesting depth, unlike a `return`/
// LocalDecl) rather than `collectLocalDeclDirectLambdaTargets`'s narrower
// 7-statement-form walk. At every CallExpr/NullCondCallExpr/NewExpr node it
// resolves the callee's formal list through the EXISTING authorities this
// file's other owned-sink classifiers already use for the identical
// question — `calleeOrChainFormals` (§3751/§3769, which itself falls back
// to a `this(...)`/`base(...)` chain target) for a CallExpr/
// NullCondCallExpr, `resolveCtorParams` for a `new T(...)` — and maps each
// DIRECT-LambdaExpr argument to its BOUND formal's resolved type via
// `boundFormalIndex` (the SAME §22.5 binder `recordMovesInArgs`/
// `markOutOwnedArgsCore` key their own classification off, so a named or
// reordered argument list is not mis-mapped) and
// `CalleeFormals::declType`/`resolvedType` (the same order
// `verifiedCallBoundedFormalAt`'s `formalIsPlainFunction` uses). No second
// argument-to-formal binding rule is introduced (CLAUDE.md §6). The result
// feeds `checkOneLambdaOwnedReturnDirectBorrow` as a THIRD `expectedFnType`
// fallback (after a direct `return` and a direct LocalDecl target) from both
// entry points (`checkLambdaTaskFamilyBorrowReturns` block-body driver and
// its `-Expr` sibling), so every position already wired for #4004's operator
// coverage and #4005's lambda coverage gets the call-argument case for free.
//
// per §28.3 *Borrow lifetime rules*, *Concrete-slot function bodies*, v0.294

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

// The SAME #4005 diagnostic — this fix supplies a THIRD source for the
// `expectedFnType` signal that check already consumes; it introduces no new
// wording.
constexpr const char* kLambdaOwnedBorrowReturn =
    "the lambda is checked against a target function type whose return is "
    "`owned`";

// A destructor-counter Res: ctor ++, dtor --, surfaced through Res.live so an
// ACCEPT test can assert exact-once disposal (0 == no leak, no double-free).
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
// REJECT — the argument-position grid. Fail-without (the r15-c3 batch with
// #4004/#4005/#4033/#4035/#4043 already applied): every one of these
// compiles with rc=0 and zero diagnostics; running under ASan double-frees
// (measured for the exact-repro row; the rest share the identical mechanism
// — a borrow tracked Owned through a callee's `owned`-declared function-type
// parameter). Pass-with (this fix): rejected at compile time with a real
// source location and this fix's diagnostic text.
// ===========================================================================

// The issue's EXACT repro: a lambda literal passed directly as a positional
// call argument to an ordinary static method's `() -> owned T` parameter.
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           direct_positional_argument_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Consume(mk: () -> owned Res): int "
        "    var c := mk(); "
        "    return c.tag; "
        "  end Consume; "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    return Consume(lambda() => b); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// Via a NAMED argument, reordered ahead of a preceding positional formal —
// exercises `boundFormalIndex`'s NAME-based binding rather than the source
// position (§22.5): `mk` is written first but binds formal index 1.
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           named_argument_reordered_rejected_spec_22_5) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Consume2(tag: int, mk: () -> owned Res): int "
        "    var c := mk(); "
        "    return c.tag + tag; "
        "  end Consume2; "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    return Consume2(mk := lambda() => b, tag := 5); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// To a CONSTRUCTOR (`new T(...)`) — exercises the `resolveCtorParams` arm
// (a NewExpr has no "callee" Expr; the formal list comes from the
// constructed type directly).
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           constructor_argument_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class Holder "
        "  public constructor(mk: () -> owned Res) "
        "    using var c := mk(); "
        "  end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var h := new Holder(lambda() => b); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// Inside an OPERATOR's own body — exercises that #4004's operator-body
// wiring (which drives `checkLambdaTaskFamilyBorrowReturns`/`-Expr` for a
// block/expression-bodied operator exactly as a method body) reaches this
// fix's new call-argument collector too, not just the return/local-decl
// ones #4004's own tests cover. `r` is captured from the SAME operator body
// (declared before the call, outside the lambda's own span).
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           operator_body_call_argument_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  public static method Consume(mk: () -> owned Res): int "
        "    var c := mk(); "
        "    return c.tag; "
        "  end Consume; "
        "  public static method operator +(a: C, b: C): int "
        "    using var r := new Res(4); "
        "    return Consume(lambda() => r); "
        "  end operator +; "
        "end C; "
        "class D "
        "  public static method Main(): int "
        "    var a := new C(1); var b := new C(2); "
        "    return a + b; "
        "  end Main; "
        "end D;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// To a GENERIC method whose parameter type is `() -> owned T` — the
// `funcReturnOwned` marker lives on the DECLARED (pre-substitution) formal
// type, so it is visible regardless of what `T` is instantiated to. `seed`
// pins `T := Res` via ordinary argument-type inference so the shape needs no
// explicit type argument.
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           generic_method_argument_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method ConsumeG<T>(seed: T, mk: () -> owned T): int "
        "    using var c := mk(); "
        "    return 0; "
        "  end ConsumeG; "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    return ConsumeG(b, lambda() => b); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// NESTED — a lambda argument inside the BODY of another lambda that is
// ITSELF a call argument (`Outer(lambda() => do return Consume(lambda() =>
// b); end)`). `collectCallArgDirectLambdaTargetsExpr`'s LambdaExpr arm
// descends into a found lambda's own body by construction, so this nesting
// needs no separate per-lambda re-invocation the way #4033's statement-only
// walker did.
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           nested_lambda_argument_inside_lambda_argument_rejected_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Consume(mk: () -> owned Res): int "
        "    var c := mk(); "
        "    return c.tag; "
        "  end Consume; "
        "  public static method Outer(f: () -> int): int "
        "    return f(); "
        "  end Outer; "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    return Outer(lambda() => do return Consume(lambda() => b); end); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// An OVERLOADED callee where only ONE overload (picked here by ARITY, per
// §22.6) takes the owned-returning slot — proves the callee resolved through
// `ctx_.exprSymbol` (already the chosen overload from sema's own resolution,
// not re-derived here) is the one this fix's formal-list lookup consults.
// The sibling `overloaded_callee_non_owned_overload_not_rejected` test below
// is this row's ACCEPT twin (same two overloads, the other arity).
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           overloaded_callee_owned_overload_rejected_spec_22_6) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Consume4(mk: () -> Res): int "
        "    var v := mk(); "
        "    return v.tag; "
        "  end Consume4; "
        "  public static method Consume4(mk: () -> owned Res, tag: int): int "
        "    var c := mk(); "
        "    return c.tag + tag; "
        "  end Consume4; "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    return Consume4(lambda() => b, 5); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// ===========================================================================
// ACCEPT — a genuinely owned production through a call argument is NOT
// over-rejected, and disposes the value EXACTLY once under the AOT+ASan
// oracle (Res.live == 0 at exit — no leak, no double-free). LOOP-AMPLIFIED
// 50x (LANE_RULES §3/§15): a one-shot run cannot distinguish "correctly
// accepted" from "accepted and leaking/double-freeing" — a per-iteration
// defect drives the final count away from 0 unambiguously either way.
// ===========================================================================

// The exact-repro shape's ACCEPT twin: a fresh `new` production.
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           direct_positional_fresh_new_accepted_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Consume(mk: () -> owned Res): int "
        "    using var c := mk(); "
        "    return c.tag; "
        "  end Consume; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 50 do "
        "      var t := Consume(lambda() => new Res(9)); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// The named-argument shape's ACCEPT twin.
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           named_argument_fresh_new_accepted_disposes_once_spec_22_5) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Consume2(tag: int, mk: () -> owned Res): int "
        "    using var c := mk(); "
        "    return c.tag + tag; "
        "  end Consume2; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 50 do "
        "      var t := Consume2(mk := lambda() => new Res(9), tag := 5); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// The constructor-argument shape's ACCEPT twin.
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           constructor_argument_fresh_new_accepted_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class Holder "
        "  public constructor(mk: () -> owned Res) "
        "    using var c := mk(); "
        "  end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 50 do "
        "      var h := new Holder(lambda() => new Res(9)); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// A relayed OWNED call result (the callee-returns-`owned`-T producer form)
// through a call argument.
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           relayed_owned_call_result_accepted_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Make(): owned Res return new Res(9); end Make; "
        "  public static method Consume(mk: () -> owned Res): int "
        "    using var c := mk(); "
        "    return c.tag; "
        "  end Consume; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 50 do "
        "      var t := Consume(lambda() => Make()); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// A `move`-discharged OWN local re-bound before being returned — a genuine
// transfer, not a bare read (§28.8) — must NOT be over-rejected by this
// fix's new formal-list lookup.
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           own_local_move_alias_accepted_disposes_once_spec_28_8) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Consume(mk: () -> owned Res): int "
        "    using var c := mk(); "
        "    return c.tag; "
        "  end Consume; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 50 do "
        "      var t := Consume(lambda() => do "
        "        var x := new Res(4); "
        "        var items := move x; "
        "        return items; "
        "      end); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// The OVERLOADED-callee ACCEPT twin: the SAME two overloads as the REJECT
// row above, called at the OTHER arity — resolves to the NON-`owned`-target
// overload, so this check must not fire at all. This check's own diagnostic
// is Severity::Error, so `jr.ran` (compile+run SUCCEEDED) is conclusive
// proof it did not fire.
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           overloaded_callee_non_owned_overload_not_rejected_spec_22_6) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Consume4(mk: () -> Res): int "
        "    var v := mk(); "
        "    return v.tag; "
        "  end Consume4; "
        "  public static method Consume4(mk: () -> owned Res, tag: int): int "
        "    var c := mk(); "
        "    return c.tag + tag; "
        "  end Consume4; "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    return Consume4(lambda() => b); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
}

// A call argument with NO owned-target context at all (a plain, non-`owned`
// function-typed parameter) must not be flagged — this check only fires when
// the BOUND formal's function type is affirmatively marked `owned`.
BEZEL_TEST(lambda_owned_return_call_arg_direct_4034,
           plain_function_typed_argument_not_rejected) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Relay(mk: () -> Res): int "
        "    var v := mk(); "
        "    return v.tag; "
        "  end Relay; "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    return Relay(lambda() => b); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
}
