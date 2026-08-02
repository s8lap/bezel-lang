// Spec-derived tests for issue #2395 — `ensureOutVarSlots`'s plain `out var`
// branch never zero-initialized a DA-exempt inline `out var` recipient's
// slot, so a read on a control-flow path where the introducing call was
// SKIPPED observed raw stack garbage instead of the §7.9 default.
//
// BACKGROUND: discovered while verifying #2345 (DefiniteAssignment gains
// per-path modeling for `?.`, mirroring `and`/`or`'s `evalCond`). #2345's own
// diff to `LlvmBackend_Calls2.cpp` adds a NEW call site for
// `ensureOutVarSlots` inside `lowerNullCondCall`; it does not touch that
// function's body, and the identical failure reproduces through plain `and`
// with no `?.` involved at all — confirming the bug PREDATES #2345 and is not
// specific to null-conditional calls.
//
// THE SPEC QUESTION (settled from the text, not the code): on a
// call-skipped path, is an inline `out var` recipient's slot REQUIRED to
// hold a default value, or merely unassigned-and-unreadable (so DA alone
// must guarantee no read reaches it)? §22.4 ("Scope is not definite
// assignment", Bezel_Language_Spec.md v0.235) answers this directly:
// "On a path where the call may not have run, a read of a
// **non-nullable-reference-typed** out variable is a compile error, exactly
// as for any other unassigned non-nullable local; value-typed and nullable
// out variables follow §7.9's default-initialization rules (the default
// value or `null` is what such a path observes)." §7.9's "Default
// initialization" subsection spells out what that default IS per kind: zero
// for numerics, `false` for `bool`, the all-default record/struct, `null`
// for a nullable reference. So the answer is BOTH, split by type: for a
// non-nullable REFERENCE recipient, DA's rejection is the WHOLE mechanism —
// no default is required or promised (LOAD-BEARING: DefiniteAssignment.cpp's
// `inlineOutVarNeedsDA`/`isNonNullableReferenceType` reject the read at
// compile time; see `and_skip_nonnull_reference_recipient_rejected_by_da`
// below, which is unaffected by this fix and would compile-reject with or
// without it). For every OTHER recipient shape — nullable reference,
// nullable value, and (confirmed empirically while verifying this issue,
// broader than its own title suggests) a plain NON-nullable value type like
// `int` — the codegen-level zero-store this fix adds is LOAD-BEARING: DA
// does not track these at all (`inlineOutVarNeedsDA` returns false), so
// nothing else in the compiler stops the read; only the store itself
// prevents it from observing garbage.
//
// THE FIX had two parts (see LlvmBackend_Calls2.cpp / LlvmBackend_Exprs2.cpp
// for the full rationale):
//   1. `ensureOutVarSlots`'s plain "out var" branch now stores
//      `llvm::Constant::getNullValue(slotTy)` right after the alloca
//      whenever `!sema::isNonNullableReferenceType(loc->declType)` — the
//      exact predicate DefiniteAssignment.cpp's `inlineOutVarNeedsDA` uses,
//      so codegen's default matches exactly what DA promises is readable.
//   2. That alone was not sufficient for `and`/`or`: `lowerShortCircuit`
//      lowers the right operand (hence calls `ensureOutVarSlots`,
//      transitively) from INSIDE the conditionally-reached "sc.rhs" block,
//      so the new store landed there too — dominating the taken-call path
//      but not the skip path. `hoistSkippableOutVarSlots` now runs over the
//      right operand BEFORE `lowerShortCircuit` emits its branch, from the
//      block that dominates both continuations, matching the placement
//      `lowerNullCondCall` already uses for `?.` (added by #2345).
//
// This bug produces a SILENT WRONG VALUE with no sanitizer signal (confirmed
// while investigating: the issue's own repro returns the expected value
// under `--sanitize=address` — ASan's shadow-memory instrumentation happens
// to mask the garbage read — but returns the WRONG value on a plain,
// unsanitized `build/bezelc -o out FILE.bzl && ./out`, the strict oracle
// `runAot` below drives). Every test in this file therefore asserts a VALUE
// (an exit code distinguishing "read the garbage/wrong-typed sentinel" from
// "read the promised default"), never just pass/fail-of-compilation.
//
// Mechanically verified fail-without/pass-with: reverted
// LlvmBackend_Calls2.cpp / LlvmBackend_Exprs2.cpp / LlvmBackend_impl.hpp to
// their pre-fix content in place (`git checkout HEAD~1 -- <paths>`, NOT git
// stash — a worktree-shared ref that races with parallel lanes), rebuilt,
// and re-ran this filter: 5 of the 9 tests below FAILED against a WRONG
// value (`and_skip_nullable_primitive...`, `and_skip_nullable_reference...`,
// `nullcond_skip_nullable_primitive...`, `and_skip_nonnull_value...`,
// `or_skip_nullable_primitive...`), pinning that those five are load-bearing
// regression coverage. `and_skip_nonnull_bool_recipient_defaults_to_false`
// happened to PASS even pre-fix in that run — raw stack garbage is
// nondeterministic (this build's entry-block layout evidently left that
// particular i1 slot zeroed by chance) — so that one test's mechanical
// fail-without proof did not hold on this run; it is kept anyway because it
// pins a THIRD LLVM lowering shape (i1, distinct from the {i8,T} nullable
// struct and the plain i32) the fix's `getNullValue(slotTy)` must also
// handle correctly, and a future stack-layout change could easily surface
// nonzero garbage there. The two taken-call controls and the non-nullable-
// reference DA-rejection control passed both before and after, as expected
// (they are not testing the skip-path defect). Restored via
// `git checkout HEAD --`, rebuilt, confirmed all 9 pass again.

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
// #2395's own repro, verbatim in shape: a nullable PRIMITIVE (`int?`)
// recipient of a plain `out var`, skipped via `and` short-circuiting
// (`flag` is `false`, so `TryParse` never runs). Expected per §7.9/§22.4: the
// slot defaults to `null`, so `n <> null` is false and Main returns 3.
// Pre-fix this returned 2 (`n <> null` read as true off garbage tag bits in
// the `{i8 hasValue, i32 value}` struct).
// =============================================================================

BEZEL_TEST(ensure_out_var_slots_2395,
           and_skip_nullable_primitive_recipient_defaults_to_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryParse(s: string, out n: int?): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public static method Main(): int "
        "    var flag := false; "
        "    if flag and C.TryParse(\"x\", out var n) then "
        "      return 1; "
        "    end; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

// Same shape, but the recipient is a nullable REFERENCE type (`string?`) —
// the garbage-pointer case the issue calls out as strictly more dangerous
// than the value case (a garbage bit pattern read as non-null could be
// DEREFERENCED as a wild pointer on a later non-null use; this test only
// checks the narrowing outcome, not a deref, but pins the same "defaults to
// null" contract for the reference shape).
BEZEL_TEST(ensure_out_var_slots_2395,
           and_skip_nullable_reference_recipient_defaults_to_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryParse(s: string, out n: string?): bool "
        "    n := \"hello\"; return true; "
        "  end TryParse; "
        "  public static method Main(): int "
        "    var flag := false; "
        "    if flag and C.TryParse(\"x\", out var n) then "
        "      return 1; "
        "    end; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

// =============================================================================
// The `?.` shape (#2345's own newly-reachable call site,
// `lowerNullCondCall`'s call to `ensureOutVarSlots`): a null receiver skips
// the call the same way `flag = false` skips it for `and` above. This
// confirms the fix's zero-store logic is shared correctly between both call
// sites (it already worked for `?.` once the store existed, since
// `lowerNullCondCall` calls `ensureOutVarSlots` BEFORE its own branch — this
// test guards against a future change disturbing that placement).
// =============================================================================

BEZEL_TEST(ensure_out_var_slots_2395,
           nullcond_skip_nullable_primitive_recipient_defaults_to_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public method TryParse(s: string, out n: int?): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public static method Main(): int "
        "    var recv: C? := null; "
        "    if recv?.TryParse(\"x\", out var n) = true then "
        "      return 1; "
        "    end; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

// =============================================================================
// Broader than the issue's own title: a plain NON-nullable VALUE-typed
// recipient (`out n: int`, no `?`) is ALSO exempt from DA tracking —
// `isNonNullableReferenceType` returns false for it (it is not a reference
// type at all), so `inlineOutVarNeedsDA` leaves it untracked on exactly the
// same "has a well-defined default" grounds §7.9 states for numerics ("zero
// for numerics"). Confirmed empirically while verifying #2395 that this
// shape was ALSO silently reading garbage pre-fix (a masked int, not just a
// masked nullable tag) — this test pins that the fix's `getNullValue`
// covers the bare scalar case too, not only the nullable-tagged-struct one.
// =============================================================================

BEZEL_TEST(ensure_out_var_slots_2395,
           and_skip_nonnull_value_recipient_defaults_to_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryParse(s: string, out n: int): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public static method Main(): int "
        "    var flag := false; "
        "    if flag and C.TryParse(\"x\", out var n) then "
        "      return 1; "
        "    end; "
        "    return n; "
        "  end Main; end C;"), 0);
}

// A `bool` recipient exercises the i1 lowering specifically (getNullValue on
// i1 must yield `false`, not an unspecified bit) — a third LLVM shape beyond
// the {i8,T} nullable struct and the plain i32.
BEZEL_TEST(ensure_out_var_slots_2395,
           and_skip_nonnull_bool_recipient_defaults_to_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryFlag(s: string, out ok: bool): bool "
        "    ok := true; return true; "
        "  end TryFlag; "
        "  public static method Main(): int "
        "    var flag := false; "
        "    if flag and C.TryFlag(\"x\", out var ok) then "
        "      return 1; "
        "    end; "
        "    if ok then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

// =============================================================================
// The `or` direction: `or`'s right operand is skipped when the LEFT is
// TRUE (the opposite polarity from `and`). Exercises `lowerShortCircuit`'s
// `isAnd=false` branch and `hoistSkippableOutVarSlots`' placement for it —
// the same hoist call handles both operators (see lowerShortCircuit), but
// this is the only test in the suite that actually drives the `or` skip
// path, so it is the one that would catch a polarity-specific regression.
// =============================================================================

BEZEL_TEST(ensure_out_var_slots_2395,
           or_skip_nullable_primitive_recipient_defaults_to_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryParse(s: string, out n: int?): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public static method Main(): int "
        "    var flag := true; "
        "    var hit := false; "
        "    if flag or C.TryParse(\"x\", out var n) then "
        "      hit := true; "
        "    end; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

// =============================================================================
// Control: the non-nullable REFERENCE-typed recipient. Per §22.4, DA rejects
// ANY read of it on a path the call may have skipped — a compile error, not
// a runtime default. This is the ONE case #2395's fix deliberately leaves
// un-stored (storing would be dead weight on an always-DA-rejected path,
// self-pass note (c) in the fix commit). Unaffected by this fix either way —
// pins that the DA rejection is still doing its job, load-bearing on its
// own, no codegen default needed.
// =============================================================================

BEZEL_TEST(ensure_out_var_slots_2395,
           and_skip_nonnull_reference_recipient_rejected_by_da) {
    auto r = runAot(
        "module M; class C "
        "  public static method TryParse(s: string, out n: string): bool "
        "    n := \"hi\"; return true; "
        "  end TryParse; "
        "  public static method Main(): int "
        "    var flag := false; "
        "    if flag and C.TryParse(\"x\", out var n) then "
        "      return 1; "
        "    end; "
        "    if n = \"hi\" then return 2; end; "
        "    return 3; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool citedUnassigned = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("read of unassigned local `n`") != std::string::npos)
            citedUnassigned = true;
    BEZEL_EXPECT_TRUE(citedUnassigned);
}

// =============================================================================
// Taken-call controls: self-pass (a) from the fix commit — zero-initializing
// the slot must NOT change codegen for the path where the call actually
// RUNS. The zero-store fires first (in the dominating block for `and`/`or`,
// or immediately before the call for a plain `.` invocation), then the real
// call overwrites it; these prove the overwrite is observed, not the zero.
// =============================================================================

BEZEL_TEST(ensure_out_var_slots_2395,
           and_taken_call_writes_real_value_not_the_zero_default) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryParse(s: string, out n: int?): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public static method Main(): int "
        "    var flag := true; "
        "    if flag and C.TryParse(\"x\", out var n) then "
        "      if n <> null and n! = 42 then return 0; end; "
        "      return 1; "
        "    end; "
        "    return 3; "
        "  end Main; end C;"), 0);
}

BEZEL_TEST(ensure_out_var_slots_2395,
           nullcond_taken_call_writes_real_value_not_the_zero_default) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public method TryParse(s: string, out n: int?): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public static method Main(): int "
        "    using var recv := new C(); "
        "    var r: C? := recv; "
        "    r?.TryParse(\"x\", out var n); "
        "    if n <> null and n! = 42 then return 0; end; "
        "    return 1; "
        "  end Main; end C;"), 0);
}
