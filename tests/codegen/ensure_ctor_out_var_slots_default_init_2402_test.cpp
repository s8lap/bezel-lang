// Spec-derived tests for issue #2402 — `ensureCtorOutVarSlots` (the
// constructor analogue of #2395's `ensureOutVarSlots`, used for
// `new T(out var x)`) never zero-initialized a DA-exempt inline `out var`
// recipient's slot, so a read on a control-flow path where the constructor
// call was SKIPPED (`and`/`or` short-circuiting) observed raw stack garbage
// instead of the §7.9 default — the identical defect #2395 fixed, on a
// different call path (`new T(...)` rather than a plain method call).
//
// THE SPEC SPLIT (re-verified against docs/Bezel_Language_Spec.md v0.235,
// same text #2395 cites — §22.4 does not distinguish a constructor call from
// a method call; both go through the same "Inline `out` declarations"
// section, and a constructor's `out`/`out owned` parameters are ordinary
// parameter-modifier declarations per §22.3/§22.7): "On a path where the
// call may not have run, a read of a **non-nullable-reference-typed** out
// variable is a compile error, exactly as for any other unassigned
// non-nullable local; value-typed and nullable out variables follow
// [§7.9](#sec-default-initialization)'s default-initialization rules (the
// default value or `null` is what such a path observes)." §7.9 spells out
// the per-kind default: zero for numerics, `false` for `bool`, the
// all-default record/struct, `null` for a nullable reference/nullable
// value's tag.
//
// THE FIX mirrors #2395 exactly, per that issue's own instruction — see
// LlvmBackend_Calls2.cpp / LlvmBackend_Exprs2.cpp / LlvmBackend_impl.hpp for
// the full rationale:
//   1. `ensureCtorOutVarSlots` now stores `llvm::Constant::getNullValue`
//      right after the alloca whenever the recipient is not a non-nullable
//      REFERENCE type — checked via `sema::isNonNullableReferenceType`
//      against the constructor PARAMETER's own freshly-resolved type (`pt`),
//      not `loc->declType` as #2395's own predicate uses for the plain-call
//      path. Reason (see #2409, filed while verifying this fix): the sema
//      registration for a CONSTRUCTOR out-var strips the declared-nullable
//      flag off `loc->declType` unconditionally — a divergence from the
//      method-call registration (fixed by #1597) that would otherwise make
//      this fix wrongly classify a NULLABLE reference ctor recipient as
//      non-nullable-reference and skip its `null` default. `pt` sidesteps
//      that bug entirely (it is resolved before any such stripping).
//   2. `hoistSkippableOutVarSlots` (added by #2395) gained a `NewExpr` case
//      (mirroring its `CallExpr` case) and a `MemberAccessExpr` case
//      (recursing into `target`) — the natural constructor-out-var shape,
//      `new Widget(...).field`, is a MemberAccessExpr wrapping the NewExpr,
//      and without the MemberAccessExpr case the NewExpr case is never
//      reached for it. The existing `CallExpr` case also gained a `callee`
//      recursion (not just `args`), for the equally natural
//      `new Widget(out var n).SomeMethod()` chaining shape.
//
// A SEPARATE, PRE-EXISTING sema bug (#2409) makes `n <> null`-style
// comparisons against a NULLABLE-VALUE constructor out-var (e.g. `int?`)
// constant-fold to `true` regardless of the actual stored value (confirmed
// via `BEZEL_DUMP_IR`: the branch is emitted as `br i1 true, ...`, with the
// real hasValue-tag extraction present but unused/dead). This is NOT this
// fix's bug (the plain-call `int?` case is unaffected — #2395's own tests
// use `n <> null` successfully) and not fixed here (different function,
// #2409 is filed separately) — but it DOES mean `n <> null` cannot be used
// to observe a nullable-VALUE ctor out-var's default in a test: it would
// read `true` unconditionally, before AND after this fix, proving nothing.
// The nullable-value tests below instead read via `n!` inside a
// `try`/`catch NullReferenceException` (confirmed via `BEZEL_DUMP_IR` to
// correctly extract and branch on the real hasValue tag, unlike `<> null`)
// — a thrown `NullReferenceException` means hasValue was correctly zeroed;
// a value flowing through means it was not.
//
// This bug produces a SILENT WRONG VALUE (or, for the nullable-reference
// case, a wild-pointer read) with no sanitizer signal on the taken path;
// every test below asserts a VALUE (an exit code distinguishing "read
// garbage" from "read the promised default"), never just pass/fail.
//
// Mechanically verified fail-without/pass-with: saved this fix's diff to
// LlvmBackend_Calls2.cpp / LlvmBackend_impl.hpp as a patch, reverted both
// files to pre-fix content with `git apply -R` (NOT git stash — a
// worktree-shared ref that races with parallel lanes), rebuilt, and re-ran
// this filter: every skip-path test below FAILED against a wrong value
// (`and_skip_nullable_primitive_via_memberaccess`,
// `and_skip_nullable_reference_via_memberaccess`,
// `and_skip_nonnull_value_via_memberaccess`,
// `and_skip_nonnull_bool_via_memberaccess`, `or_skip_nullable_primitive`,
// `and_skip_nullable_primitive_via_method_chain`) while the two taken-call
// controls stayed passing (they exercise the always-correct written-value
// path, not the skip-path default). Then re-applied the patch (`git apply`)
// and rebuilt to confirm all tests pass again.

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// =============================================================================
// The issue's own repro, verbatim in shape: a nullable PRIMITIVE (`int?`)
// constructor out-var recipient, skipped via `and` short-circuiting (`flag`
// is `false`, so the constructor's `new Widget(...)` call never runs), read
// through a MemberAccessExpr chain (`.ok`) exactly as the issue reports it —
// this is also what exercises `hoistSkippableOutVarSlots`'s new
// MemberAccessExpr case, without which the NewExpr case is never reached for
// this shape at all. Verified via `n!` + try/catch (see file header: `<>
// null` is unusable here due to the separate #2409 constant-fold bug).
// Expected per §7.9/§22.4: the slot defaults to `null`, so `n!` throws
// NullReferenceException and Main returns 3. Pre-fix this returned 2 (`n!`
// read a garbage-nonzero hasValue tag as present, returning a garbage int
// instead of throwing).
// =============================================================================

BEZEL_TEST(ensure_ctor_out_var_slots_2402,
           and_skip_nullable_primitive_via_memberaccess_defaults_to_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: int?) "
        "    n := 7; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var flag := false; "
        "    if flag and new Widget(\"x\", out var n).ok then "
        "      return 1; "
        "    end; "
        "    try var v := n!; return 2; "
        "    catch NullReferenceException => return 3; end; "
        "  end Main; end C;"), 3);
}

// Same shape, but the recipient is a nullable REFERENCE type (`string?`).
// Unlike the nullable-value case, `<> null` is NOT confounded by #2409 for a
// reference type (that bug is specific to the nullable-VALUE tagged-struct
// null-check path) — confirmed this assertion actually distinguishes
// garbage-pointer-as-non-null (pre-fix, exit 2) from a correct null read
// (post-fix, exit 3) via the mechanical revert below.
BEZEL_TEST(ensure_ctor_out_var_slots_2402,
           and_skip_nullable_reference_via_memberaccess_defaults_to_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string?) "
        "    n := \"hello\"; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var flag := false; "
        "    if flag and new Widget(\"x\", out var n).ok then "
        "      return 1; "
        "    end; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

// =============================================================================
// Broader than "nullable": a plain NON-nullable VALUE-typed recipient
// (`out n: int`, no `?`) is ALSO exempt from DA tracking (it is not a
// reference type at all, so `isNonNullableReferenceType` is false for it
// too) — confirmed empirically to be silently reading garbage pre-fix, the
// same as #2395 found for the plain-call path. `<> `-style comparisons don't
// apply to a bare non-nullable `int`; read the value directly instead.
// =============================================================================

BEZEL_TEST(ensure_ctor_out_var_slots_2402,
           and_skip_nonnull_value_via_memberaccess_defaults_to_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: int) "
        "    n := 42; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var flag := false; "
        "    if flag and new Widget(\"x\", out var n).ok then "
        "      return 1; "
        "    end; "
        "    return n; "
        "  end Main; end C;"), 0);
}

// A `bool` recipient exercises the i1 lowering specifically (getNullValue on
// i1 must yield `false`) — a distinct LLVM shape from the plain i32 case.
BEZEL_TEST(ensure_ctor_out_var_slots_2402,
           and_skip_nonnull_bool_via_memberaccess_defaults_to_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out flag2: bool) "
        "    flag2 := true; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var flag := false; "
        "    if flag and new Widget(\"x\", out var flag2).ok then "
        "      return 1; "
        "    end; "
        "    if flag2 then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

// =============================================================================
// The `or` direction: `or`'s right operand (the `new Widget(...)` call) is
// skipped when the LEFT is TRUE — the mirror polarity of `and`. Exercises
// `lowerShortCircuit`'s `isAnd=false` branch calling the (shared)
// `hoistSkippableOutVarSlots` for a constructor out-var specifically.
// =============================================================================

BEZEL_TEST(ensure_ctor_out_var_slots_2402,
           or_skip_nullable_primitive_via_memberaccess_defaults_to_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: int?) "
        "    n := 7; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var flag := true; "
        "    var hit := false; "
        "    if flag or new Widget(\"x\", out var n).ok then "
        "      hit := true; "
        "    end; "
        "    try var v := n!; return 2; "
        "    catch NullReferenceException => return 3; end; "
        "  end Main; end C;"), 3);
}

// =============================================================================
// The method-CHAIN shape: `new Widget(out var n).SomeMethod()`, a CallExpr
// whose CALLEE (not just its args) is a MemberAccessExpr targeting the
// NewExpr. Exercises the `callee` recursion added to
// `hoistSkippableOutVarSlots`'s existing CallExpr case — without it, this
// shape's nested NewExpr is never reached and the fix is a no-op for it.
// =============================================================================

BEZEL_TEST(ensure_ctor_out_var_slots_2402,
           and_skip_nullable_primitive_via_method_chain_defaults_to_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public constructor(s: string, out n: int?) "
        "    n := 7; "
        "  end; "
        "  public method Ok(): bool "
        "    return true; "
        "  end Ok; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var flag := false; "
        "    if flag and new Widget(\"x\", out var n).Ok() then "
        "      return 1; "
        "    end; "
        "    try var v := n!; return 2; "
        "    catch NullReferenceException => return 3; end; "
        "  end Main; end C;"), 3);
}

// =============================================================================
// Taken-call controls: self-pass — zero-initializing the slot must NOT
// change codegen for the path where the constructor actually RUNS. The
// zero-store fires first (in the dominating entry block), then the real
// constructor call overwrites it; these prove the overwrite is observed, not
// the zero, for both the `.field` and the method-chain shape.
// =============================================================================

BEZEL_TEST(ensure_ctor_out_var_slots_2402,
           and_taken_call_writes_real_value_not_the_zero_default) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: int?) "
        "    n := 42; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var flag := true; "
        "    if flag and new Widget(\"x\", out var n).ok then "
        "      try var v := n!; if v = 42 then return 0; end; return 1; "
        "      catch NullReferenceException => return 4; end; "
        "    end; "
        "    return 3; "
        "  end Main; end C;"), 0);
}

BEZEL_TEST(ensure_ctor_out_var_slots_2402,
           and_taken_call_method_chain_writes_real_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public constructor(s: string, out n: int?) "
        "    n := 42; "
        "  end; "
        "  public method Ok(): bool "
        "    return true; "
        "  end Ok; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var flag := true; "
        "    if flag and new Widget(\"x\", out var n).Ok() then "
        "      try var v := n!; if v = 42 then return 0; end; return 1; "
        "      catch NullReferenceException => return 4; end; "
        "    end; "
        "    return 3; "
        "  end Main; end C;"), 0);
}
