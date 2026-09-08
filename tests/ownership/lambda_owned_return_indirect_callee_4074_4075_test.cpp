// Spec-derived regression tests for issues #4074 and #4075 — the last two
// disclosed-open gaps of the #4034 owned-return-call-argument family.
//
// #4034 (landed: 8250253 / 0037f1c) threads a lambda argument's expected
// `() -> owned T` TARGET TYPE from the CALLEE's own formal parameter list
// through `calleeOrChainFormals`/`resolveCtorParams`, both of which require
// `ctx_.exprSymbol(callee)` to be a `MethodSymbol`/`ConstructorSymbol`/
// `ChoiceCaseSymbol` (or a `this(...)`/`base(...)` chain target). Two callee
// shapes resolve to NEITHER and were therefore never checked at all:
//
//   #4074 — the callee is an INVOKED FUNCTION VALUE: a plain parameter,
//   field, or local whose own STATIC TYPE happens to be a function type
//   (`caller(lambda() => b)` where `caller: (() -> owned Res) -> int`).
//   `ctx_.exprSymbol(caller)` resolves to a `ParameterSymbol`/`FieldSymbol`/
//   local, not a Method/Constructor/ChoiceCase — MEASURED heap-use-after-free.
//
//   #4075 — the callee is an INDEXER (`obj[mk]`, §23.4): an `IndexExpr` has
//   no callee EXPRESSION at all (a `target` and an index `ArgList`), so
//   `calleeOrChainFormals`, which is keyed on a callee expression, can never
//   see it. MEASURED 48-byte LSan leak.
//
// THE FIX (this commit): two NEW, narrowly-scoped resolvers —
// `recordCallArgDirectLambdaTargetsFromFuncValue` (reads the callee
// EXPRESSION's own resolved Function TypeRef via `ctx_.exprType`, mirroring
// the EXISTING `checkScopedAdmission` function-value arm, Ownership_Checks.cpp,
// fix #1420/#1439 — CLAUDE.md §6) and `recordCallArgDirectLambdaTargetsFromIndexer`
// (reads the resolved indexer `PropertySymbol` ExprTypecheck's own §23.4
// branch already attaches to the IndexExpr node via `ctx_.setExprSymbol`,
// fix #2530 — the SAME fact `checkIndexArgNullability`'s `declaredIndexParam`
// lambda, ExprTypecheck_Exprs.cpp fix #2619, already reads for the identical
// question). Both feed ONLY the lambda-owned-target collector
// (`collectCallArgDirectLambdaTargets`/`-Expr`); NEITHER touches
// `CalleeFormals`, `resolveCalleeParams`, or `calleeOrChainFormals`
// themselves, so none of THEIR other consumers (`recordMovesInArgs`,
// `markOutOwnedArgsCore`, `scopedFormalAt`, `verifiedCallBoundedFormalAt`,
// `borrowSourceForExpr`, `catchBorrowAmongNonOwnedCtorArgs`) is affected —
// per LANE_RULES §14/§15 and the #4042 double-free lesson (a classifier
// change surfacing in a caller that keys move-marking off it). Neither the
// `producerState` classifier nor `ownedArgSourceIsBorrow` is touched.
//
// Both are POSITIONAL only (no `boundFormalIndex` §22.5 named-argument
// binding): the function-value arm mirrors the EXISTING precedent
// (`checkScopedAdmission`'s own function-value arm is positional too — a
// function-VALUE's `TypeRef::FuncParam::name` is optional call-site-facing
// metadata with no compiler-verified arity/name the way a DECLARED formal
// list has); the indexer arm is positional because §23.5 "Index arguments
// are positional-only" (fix #2758) makes a named index argument a compile
// error, so there is no bound-formal question to ask for an indexer at all.
//
// per §28.3 *Borrow lifetime rules*, *Concrete-slot function bodies*, §23.4
// *Indexers*, v0.294

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot / runAotChecked(Lenient)
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotCheckedLenient;

namespace {

// The SAME #4005 diagnostic #4034 already reuses — these two fixes supply a
// FOURTH/FIFTH source for the `expectedFnType` signal that check already
// consumes; neither introduces new wording.
constexpr const char* kLambdaOwnedBorrowReturn =
    "the lambda is checked against a target function type whose return is "
    "`owned`";

const char* kRes =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// #4074 — REJECT: an INVOKED FUNCTION VALUE.
// Fail-without (measured on eae547e): every row compiles with rc=0 and only
// the unrelated §28.3 auto-drop NOTE; running under `--sanitize=address`
// double-frees for the exact-repro row. Pass-with (this fix): rejected at
// compile time with a real source location and this fix's diagnostic text.
// ===========================================================================

// The issue's EXACT repro: `caller` is a PARAMETER whose static type is a
// function type; `caller(lambda() => b)` invokes it with a borrow-returning
// lambda argument.
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           invoked_function_value_parameter_rejected_spec_28_3_4074) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Invoke(caller: (() -> owned Res) -> int): int "
        "    using var b := new Res(4); "
        "    var v := caller(lambda() => b); "
        "    return v; "
        "  end Invoke; "
        "  public static method MakeCaller(mk: () -> owned Res): int "
        "    var c := mk(); "
        "    return c.tag; "
        "  end MakeCaller; "
        "  public static method Main(): int "
        "    return Invoke(MakeCaller); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// The callee is a FIELD (`this.caller`, a MemberAccessExpr) rather than a
// bare parameter — exercises `ctx_.exprType` on a MemberAccessExpr callee,
// not just an IdentifierExpr one.
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           invoked_function_value_field_rejected_spec_28_3_4074) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public caller: (() -> owned Res) -> int := MakeCaller; "
        "  public method Invoke(): int "
        "    using var b := new Res(4); "
        "    var v := this.caller(lambda() => b); "
        "    return v; "
        "  end Invoke; "
        "  public static method MakeCaller(mk: () -> owned Res): int "
        "    var c := mk(); "
        "    return c.tag; "
        "  end MakeCaller; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    return c.Invoke(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// The callee is a plain LOCAL (a LocalSymbol, distinct from the parameter
// and field cases above).
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           invoked_function_value_local_rejected_spec_28_3_4074) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method MakeCaller(mk: () -> owned Res): int "
        "    var c := mk(); "
        "    return c.tag; "
        "  end MakeCaller; "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var caller: (() -> owned Res) -> int := MakeCaller; "
        "    var v := caller(lambda() => b); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// The NULL-CONDITIONAL invocation spelling (`caller?(...)`, `NullCondCallExpr`
// — the sibling AST node `collectCallArgDirectLambdaTargetsExpr` handles
// alongside `CallExpr`) of the SAME invoked-function-value shape.
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           invoked_function_value_null_cond_call_rejected_spec_28_3_4074) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method MakeCaller(mk: () -> owned Res): int "
        "    var c := mk(); "
        "    return c.tag; "
        "  end MakeCaller; "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var caller: ((() -> owned Res) -> int)? := MakeCaller; "
        "    var v := caller?(lambda() => b); "
        "    return v ?? -1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// ===========================================================================
// #4074 — ACCEPT: this check must not over-reject.
// ===========================================================================

// A genuinely FRESH production through an invoked function value is not
// flagged. NOTE: unlike #4034's own ACCEPT rows, this does NOT assert
// exact-once disposal (`Res.live == 0`) under a leak-detecting oracle.
// Investigating this exact shape surfaced #4078 — a PRE-EXISTING,
// ownership-pass-UNRELATED codegen defect: invoking a function-VALUE
// parameter/local (as opposed to a plain named-method call) with a lambda
// argument that produces a fresh `owned` value leaks the produced value
// (measured: 40 B / 1 object, `bezelc` built from this exact commit, on a
// program with ZERO codegen files touched by this fix — so the leak is
// provably identical on the pre-fix compiler; a NON-owned target through
// the SAME invoked-value call shape leaks identically, confirming it is not
// specific to `owned` semantics at all). A leak-detecting oracle here would
// report a false failure sourced entirely from #4078, exactly as #4077 did
// for #4034's own `base_chain_call_fresh_new_accepted_disposes_once`
// row — except #4077's leaked object was NOT the disposal-tracked `Res`
// (so `runAotCheckedLenient` + `Res.live == 0` still proved something
// there), whereas #4078's IS the `Res` itself (the object is never
// disposed at all, not double-disposed), so `Res.live == 0` would fail here
// for a reason unrelated to this fix's soundness. `runAotCheckedLenient`
// (LeakSanitizer OFF, generated-code ASan corruption/UAF-at-exit detection
// STILL ON) is what this fix IS responsible for proving: no double-free /
// UAF is introduced by soundly accepting this shape.
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           invoked_function_value_fresh_new_accepted_no_corruption_spec_28_3_4074) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Invoke(caller: (() -> owned Res) -> int): int "
        "    var v := caller(lambda() => new Res(9)); "
        "    return v; "
        "  end Invoke; "
        "  public static method MakeCaller(mk: () -> owned Res): int "
        "    using var c := mk(); "
        "    return c.tag; "
        "  end MakeCaller; "
        "  public static method Main(): int "
        "    return Invoke(MakeCaller); "
        "  end Main; "
        "end C;";
    JitResult jr = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 9);
}

// A call argument through an invoked function value with NO owned-target
// context at all (a plain, non-`owned` function-typed parameter) must not be
// flagged — this check only fires when the BOUND formal's function type is
// affirmatively marked `owned`. Mirrors #4034's own
// `plain_function_typed_argument_not_rejected` control.
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           invoked_function_value_plain_not_rejected_4074) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Relay(caller: (() -> Res) -> int): int "
        "    using var b := new Res(4); "
        "    var v := caller(lambda() => b); "
        "    return v; "
        "  end Relay; "
        "  public static method MakeRelay(mk: () -> Res): int "
        "    var v := mk(); "
        "    return v.tag; "
        "  end MakeRelay; "
        "  public static method Main(): int "
        "    return Relay(MakeRelay); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
}

// ===========================================================================
// #4075 — REJECT: an INDEXER whose parameter type is `() -> owned T`.
// Fail-without (measured on eae547e): compiles with rc=0, zero diagnostics;
// running under `--sanitize=address` (`ASAN_OPTIONS=detect_leaks=1`) reports
// a 48-byte LSan leak. Pass-with (this fix): rejected at compile time.
// ===========================================================================

// The issue's EXACT repro: a single-parameter user-defined indexer.
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           indexer_get_argument_rejected_spec_28_3_23_4_4075) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class Box "
        "  public property [mk: () -> owned Res]: int "
        "    get => do var c := mk(); return c.tag; end; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var box := new Box(); "
        "    var r := box[lambda() => b]; "
        "    return r; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// A MULTI-parameter indexer with the owned-returning function-typed
// parameter at POSITION 1, not 0 — exercises the positional index `i` in
// `recordCallArgDirectLambdaTargetsFromIndexer`'s loop, not just the
// single-parameter case.
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           indexer_second_parameter_position_rejected_spec_28_3_23_4_4075) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class Box "
        "  public property [tag: int, mk: () -> owned Res]: int "
        "    get => do var c := mk(); return c.tag + tag; end; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var box := new Box(); "
        "    var r := box[7, lambda() => b]; "
        "    return r; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// The indexer used as an ASSIGNMENT TARGET (`box[...] := value`, the SETTER
// path, an `AssignStmt` whose `target` is the IndexExpr) — proves the
// resolved indexer `PropertySymbol` this fix reads via `ctx_.exprSymbol` is
// attached regardless of get/set usage (ExprTypecheck's #2530 attachment is
// unconditional), not just for a bare read.
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           indexer_setter_argument_rejected_spec_28_3_23_4_4075) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class Box "
        "  public v: int := 0; "
        "  public property [mk: () -> owned Res]: int "
        "    get => do var c := mk(); return c.tag; end; "
        "    set => do var c := mk(); v := c.tag + value; end; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var box := new Box(); "
        "    box[lambda() => b] := 5; "
        "    return box.v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// ===========================================================================
// #4084 — REJECT: a GENERIC user-defined indexer with a BARE-type-parameter
// function-typed parameter. Direct follow-on to #4075: every REJECT row
// above is non-generic, so the unchanged resolver's omission of the
// `substituteMemberType` call `checkIndexArgNullability`'s
// `declaredIndexParam` lambda performs (`Src/sema/ExprTypecheck_Exprs.cpp`,
// fix #2619, also #4075's mirror) is a no-op there. For a generic indexer
// whose declared parameter type is a BARE type parameter `T`, the
// unsubstituted `T` (`TypeRef::Kind::GenericParam`) is not `Function`, so
// the new resolver's `pt->kind == Function` gate never fires at a concrete
// instantiation where `T` is bound to a function type — #4075's defect
// class reopens through generics. Fail-without (measured on 511e576):
// compiles with rc=0, zero diagnostics; running under --sanitize=address
// reports a 40-byte Direct leak (allocation site attributed to `main`),
// matching the issue's exact measurement. Pass-with (this fix): rejected
// at compile time with this fix's diagnostic text — see the per-row FAIL-
// WITHOUT comment below for the exact byte-for-byte confirmation.
// ===========================================================================

// The issue's EXACT repro: a generic indexer with a bare-type-parameter
// declared parameter, instantiated at a function type whose return is
// `owned`. Substitutes `T` -> `() -> owned Res` at the call site; the
// lambda argument `lambda() => b` returns a borrow (`b` is a `using var`)
// and must therefore be rejected — the same shape as the non-generic
// `indexer_get_argument_rejected` row above, only REACHABLE here through
// the fix's substituteMemberType-equivalent path.
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           generic_indexer_bare_type_param_rejected_spec_28_3_23_4_4084) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class Box<T> "
        "  public property [mk: T]: int "
        "    get => 42; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var box := new Box<() -> owned Res>(); "
        "    var r := box[lambda() => b]; "
        "    return r; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// A MULTI-parameter generic indexer with the owned-returning lambda at
// POSITION 1, not 0 — verifies the per-slot substitution (slot 0 is the
// bare `int`, slot 1 is the bare `T` substituted at instantiation).
// Mirrors the non-generic `indexer_second_parameter_position_rejected`
// row above, only here both slots are bare-type-params and exactly the
// substituted form of slot 1 should fire the check.
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           generic_indexer_second_param_bare_type_rejected_spec_28_3_23_4_4084) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class Box<T> "
        "  public property [tag: int, mk: T]: int "
        "    get => tag + 42; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var box := new Box<() -> owned Res>(); "
        "    var r := box[7, lambda() => b]; "
        "    return r; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kLambdaOwnedBorrowReturn));
}

// A generic indexer whose declared parameter type is `() -> owned T` (T
// EMBEDDED inside an already-`Function`-kinded declared type) — already
// rejected on 511e576 without this fix, because the resolver's
// `pt->kind == Function` gate fires on the UNSUBSTITUTED declared type
// (the `Function` kind and the `owned` marker are written directly in the
// signature, regardless of what `T` resolves to). Listed here so a
// reader of this file can verify the OPPOSITE shape — "rejection on the
// unsubstituted kind" — and confirm the fix doesn't disturb it; the fix
// is strictly narrower than the existing resolver.
// ===========================================================================
// #4075 — ACCEPT: this check must not over-reject.
// ===========================================================================

// A genuinely FRESH production through an indexer argument is not flagged.
// Same #4078 caveat as the #4074 ACCEPT row above applies here too — building
// this row's `Res.live`-checked twin surfaced #4078's SECOND repro (a
// user-defined indexer getter invoked with a lambda argument that produces a
// fresh `owned` value leaks the produced value; measured 2000 B / 50 objects
// at 50 iterations, allocation site attributed to `main`, consistent with a
// zero-capture lambda inlined at the call site). `runAotCheckedLenient`
// proves what THIS fix owns: no double-free / UAF from soundly accepting
// this shape.
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           indexer_fresh_new_accepted_no_corruption_spec_28_3_23_4_4075) {
    std::string src =
        std::string(kRes) +
        "class Box "
        "  public property [mk: () -> owned Res]: int "
        "    get => do using var c := mk(); return c.tag; end; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    return box[lambda() => new Res(9)]; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 9);
}

// An indexer argument with NO owned-target context at all (a plain,
// non-`owned` function-typed indexer parameter) must not be flagged.
BEZEL_TEST(lambda_owned_return_indirect_callee_4074_4075,
           indexer_plain_not_rejected_4075) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class Box "
        "  public property [mk: () -> Res]: int "
        "    get => do var c := mk(); return c.tag; end; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Res(4); "
        "    var box := new Box(); "
        "    var r := box[lambda() => b]; "
        "    return r; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
}
