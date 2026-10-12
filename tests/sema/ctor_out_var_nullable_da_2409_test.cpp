// Spec-derived tests for issue #2409 — the constructor (`NewExpr`) inline
// `out var` / `out owned var` registration in ExprTypecheck_Exprs.cpp
// (~line 5069-5133) never mirrored #1597, which fixed the equivalent
// METHOD-call (`CallExpr`) registration (~line 4321-4382). Two distinct,
// confirmed-live consequences, both re-verified from scratch against current
// main before writing this fix (per this project's VERIFY-FIRST discipline):
//
//   (1) It never set `outVarNeedsDefiniteAssignment`, so
//       `DefiniteAssignment.cpp`'s `inlineOutVarNeedsDA` never rejected a
//       read of a non-nullable-REFERENCE ctor out-var on a path where the
//       introducing `new` call may not have run. Per §22.4 (v0.235,
//       "Inline `out` declarations"): "On a path where the call may not
//       have run, a read of a **non-nullable-reference-typed** out variable
//       is a compile error, exactly as for any other unassigned
//       non-nullable local; value-typed and nullable out variables follow
//       [§7.9]'s default-initialization rules." The equivalent plain-call
//       repro IS correctly rejected today; the constructor repro compiled
//       CLEAN and then SEGFAULTED at runtime (confirmed live, no sanitizer
//       needed, comparing a garbage stack pointer as a string) — a strictly
//       worse outcome than a missing default, since it defeats the very
//       compile-time check §22.4 exists to provide.
//
//   (2) It unconditionally STRIPPED the declared-nullable flag off
//       `lsym->declType` (the pre-#1597 CallExpr behavior, never updated
//       when constructor out-vars gained this same inline form). A nullable
//       ctor out-var (`out n: T?`) was therefore typed as plain non-nullable
//       `T`, silently permitting an implicit `T? -> T` narrowing
//       (`var x: T := n;`) that would be a compile error against a genuine
//       `T?` per §7. This ALSO cascades into codegen for a nullable-VALUE
//       (tagged-struct) recipient: `n <> null` constant-folds to `br i1
//       true, ...` regardless of the actual stored hasValue tag (confirmed
//       via BEZEL_DUMP_IR, both before this fix and independently
//       reproduced here) — LlvmBackend_Exprs2.cpp's nullable-value
//       `eitherNull` branch keys off `ctx_.exprType`, which reflected the
//       same stripped type. A nullable-REFERENCE recipient's `<> null` is
//       UNCONFOUNDED by this codegen bug (a raw-pointer null check does not
//       consult the sema-recorded nullability at all) — re-verified live via
//       a pre-fix probe before writing this file's reference-type tests, so
//       only the nullable-VALUE tests below use `<> null` as a genuine
//       fail-without/pass-with runtime-VALUE oracle; the reference-type
//       tests instead assert the TYPE-CHECK-level consequence directly
//       (an implicit narrowing / unguarded-access diagnostic), which IS the
//       observable, mechanically-provable signal for that case (there is no
//       runtime value difference for a reference recipient — codegen's
//       pointer lowering was always independent of the sema TypeRef, exactly
//       as #1597's own commit message notes for the method-call path).
//
// THE FIX mirrors #1597 in TWO files (a second file was required — found
// during this issue's own verification, not itself filed separately, since
// it is necessary for consequence (1) above to be fixable at all):
//
//   a. ExprTypecheck_Exprs.cpp's NewExpr `out var` / `out owned var` blocks:
//      stop stripping declared nullability (both blocks), and set
//      `outVarNeedsDefiniteAssignment = isNonNullableReferenceType(...)` on
//      the plain `out var` block ONLY — mirroring #1597's own CallExpr
//      `out owned var` block, which deliberately does NOT set that flag
//      (DefiniteAssignment.cpp's `visitCallArg` only ever consults it when
//      `a.modifier == "out var"` literally; the owning inline forms carry a
//      per-path disposal obligation tracked by the OWNERSHIP pass instead,
//      per that file's own comment).
//
//   b. DefiniteAssignment.cpp's generic `visitExpr` NewExpr case (~line
//      1301): routed each constructor argument through `visitCallArg` —
//      exactly as the CallExpr case does for a method call's arguments —
//      instead of a bare `visitExpr(*a.value)` per argument. Root cause:
//      `inlineOutVarNeedsDA` (and the `assigned_`/`declared_` bookkeeping
//      that makes a later out-of-scope read diagnosable at all) is ONLY
//      ever consulted from inside `visitCallArg`, which the NewExpr case
//      never called — so fix (a) ALONE left consequence (1) live: verified
//      empirically by applying fix (a) in isolation and re-running the
//      segfault repro, which still compiled clean. Without this second
//      piece, a ctor out-var's identifier was never registered with the DA
//      pass at all (its declaring occurrence and every later read both fell
//      through to the generic `IdentifierExpr` leaf, `checkRead`, which is a
//      silent no-op for any name not in `declared_`).
//
// SELF-PASS (a): does removing the strip let #2402's codegen zero-store key
// on `loc->declType` after all? Re-derived analytically (not by editing
// #2402's code, which this project's discipline says not to touch without
// proof): `ensureCtorOutVarSlots`' `pt` is `ctx_.resolvedType(*ctorDecl->
// params[pos].type)` — the SAME resolution `lsym->declType` now receives
// unmodified (post this fix, the assignment is `lsym->declType =
// ctx_.resolvedType(*ctorDecl->params[posn].type);` with no further
// mutation). So yes — `loc->declType` and `pt` are now equivalent for this
// case, and #2402's own comment explaining why it deliberately used `pt`
// instead of `loc->declType` is now describing dead-but-still-correct
// defensive code, not a live divergence. #2402's file is NOT touched here
// (small-PR discipline; `pt` remains correct, just no longer strictly
// necessary) — a possible follow-up simplification, not undertaken.
//
// MECHANICALLY VERIFIED fail-without/pass-with: saved this fix's diff
// (ExprTypecheck_Exprs.cpp + DefiniteAssignment.cpp) as a patch, reverted
// both files with `git apply -R` (NOT git stash — a worktree-shared ref that
// races with parallel lanes), rebuilt bezelc, and re-ran every test below by
// hand as a standalone .bzl probe: every DA-half reject test compiled CLEAN
// (and the `and`-skip one SEGFAULTED at runtime, matching the issue's own
// report) and every nullability-half test that must now be REJECTED
// compiled CLEAN instead (the nullable-primitive `<> null` constant-fold
// test returned the WRONG branch, exit 2 instead of 3). Then re-applied the
// patch, rebuilt, and confirmed every test below passes. The taken-call
// controls and the non-nullable-VALUE control passed identically before and
// after (proving neither fix over-restricts the unaffected paths).

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const std::string& prog, const std::string& needle) {
    auto r = runAot(prog);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// =============================================================================
// PART 1 — the DA half (§22.4 / consequence 1): a non-nullable-REFERENCE
// ctor out-var read on a path where the `new` call may not have run must be
// a COMPILE ERROR, asserting the specific diagnostic text (not "any error",
// which would also match an unrelated failure and prove nothing).
// =============================================================================

// The issue's own repro, verbatim in shape: `flag` is `false`, so
// `new Widget("x", out var n)` inside the `and` never runs; the later read
// of `n` must be rejected with DA's "read of unassigned local" diagnostic —
// pre-fix this compiled clean and SEGFAULTED at runtime instead (confirmed
// live via `git apply -R` + rebuild + run, no sanitizer needed).
BEZEL_TEST(ctor_out_var_da_2409,
           and_skip_nonnullable_reference_read_rejected_spec_22_4) {
    BEZEL_EXPECT_TRUE(hasMsg(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := \"hi\"; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var flag := false; "
        "    if flag and new Widget(\"x\", out var n).ok then "
        "      return 1; "
        "    end; "
        "    if n = \"hi\" then return 2; end; "
        "    return 3; "
        "  end Main; end C;",
        "read of unassigned local `n`"));
}

// The `or` mirror: `or`'s right operand runs only when the LEFT is false;
// here `flag` is `true`, so the constructor call is skipped and `n` is
// unassigned on the fall-through path. Exercises `evalCond`'s `or` branch
// specifically, not just `and`.
BEZEL_TEST(ctor_out_var_da_2409,
           or_skip_nonnullable_reference_read_rejected_spec_22_4) {
    BEZEL_EXPECT_TRUE(hasMsg(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := \"hi\"; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var flag := true; "
        "    var hit := false; "
        "    if flag or new Widget(\"x\", out var n).ok then "
        "      hit := true; "
        "    end; "
        "    if n = \"hi\" then return 2; end; "
        "    return 3; "
        "  end Main; end C;",
        "read of unassigned local `n`"));
}

// =============================================================================
// Controls: the DA fix must NOT newly reject paths §22.4 admits.
// =============================================================================

// Control: a non-nullable VALUE-typed recipient (`out n: int`, no `?`) is
// exempt from DA tracking altogether (`isNonNullableReferenceType` is false
// for a value type) — §7.9's zero default applies on the skip path instead
// of a compile error. This proves routing NewExpr args through
// `visitCallArg` (fix b) does not newly over-reject a value-typed skip-path
// read; the ACTUAL zero-value is supplied by #2402's separate codegen fix
// (already landed), exercised here as an integration check.
BEZEL_TEST(ctor_out_var_da_2409,
           and_skip_nonnullable_value_read_accepted_default_zero_spec_7_9) {
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

// Taken-call control: the constructor actually RUNS (`flag` is `true`), so
// `n` is definitely assigned on every reaching read — must stay ACCEPTED and
// observe the real written value, proving the DA fix does not disturb the
// ordinary (non-short-circuited) path.
BEZEL_TEST(ctor_out_var_da_2409,
           and_taken_nonnullable_reference_read_accepted_real_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := \"hi\"; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var flag := true; "
        "    if flag and new Widget(\"x\", out var n).ok then "
        "      if n = \"hi\" then return 2; end; "
        "      return 1; "
        "    end; "
        "    return 3; "
        "  end Main; end C;"), 2);
}

// A plain (non-short-circuited) constructor call — no `and`/`or` at all —
// must be entirely unaffected: the out-var is definitely assigned right
// after the call, ordinary straight-line DA.
BEZEL_TEST(ctor_out_var_da_2409,
           plain_call_nonnullable_reference_read_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := \"hi\"; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    if n = \"hi\" then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 2);
}

// =============================================================================
// PART 2 — the nullability half (consequence 2): a NULLABLE ctor out-var
// must keep its declared `T?` type, not be silently narrowed to `T`.
// =============================================================================
//
// Nullable PRIMITIVE (`int?`): a genuine RUNTIME VALUE oracle is available
// here because the strip's cascading codegen bug (constant-folding
// `n <> null` to `true`) is specific to the nullable-VALUE tagged-struct
// path (re-verified via BEZEL_DUMP_IR both pre- and post-fix while writing
// this file) — so these two tests mechanically distinguish "sema believes
// n is non-nullable" (both branches read as `true`) from "sema/codegen
// correctly track n's real hasValue tag" (the two branches diverge).

// Ctor writes a real (non-null) value: `n <> null` must read the TRUE
// branch and `n!` must yield the actual stored value, not a default/garbage
// one — proves the taken/has-value path is unaffected by the fix.
BEZEL_TEST(ctor_out_var_da_2409,
           nullable_primitive_has_value_reads_real_value_spec_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: int?) "
        "    n := 42; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    if n <> null then return n!; end; "
        "    return -1; "
        "  end Main; end C;"), 42);
}

// Ctor writes `null`: `n <> null` must read the FALSE branch. This is the
// KEY fail-without/pass-with test for the constant-fold consequence —
// pre-fix this returned 2 (the branch was unconditionally folded `true`,
// independent of the actually-stored `null`); post-fix it correctly
// returns 3. Mechanically confirmed via the patch-revert procedure above.
BEZEL_TEST(ctor_out_var_da_2409,
           nullable_primitive_null_value_reads_false_branch_spec_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: int?) "
        "    n := null; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    if n <> null then return 2; end; "
        "    return 3; "
        "  end Main; end C;"), 3);
}

// =============================================================================
// Nullable REFERENCE (`string?`): no runtime value differs pre/post-fix
// (codegen's pointer lowering was always independent of the sema TypeRef —
// re-verified live), so the mechanically-provable signal here is the
// TYPE-CHECK-level consequence directly: an implicit `T? -> T` narrowing,
// and an unguarded nullable-member-access, must both newly become compile
// errors citing the specific §7/§7.4 diagnostic (not "any error").
// =============================================================================

// The issue's own second repro, verbatim: `var x: int := n;` against a
// nullable-int ctor out-var is an implicit non-nullable narrowing — must be
// rejected citing §7. Pre-fix this compiled clean (confirmed live).
BEZEL_TEST(ctor_out_var_da_2409,
           nullable_primitive_implicit_narrowing_rejected_spec_7) {
    BEZEL_EXPECT_TRUE(hasMsg(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: int?) "
        "    n := 5; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    var x: int := n; "
        "    return x; "
        "  end Main; end C;",
        "nullable but the target type is non-nullable"));
}

// The reference-type analogue via §7.4 instead: an UNGUARDED member access
// on a nullable-string ctor out-var must be rejected — pre-fix this
// compiled clean (sema believed `n` was plain non-nullable `string`,
// silently permitting a real null-deref hazard; confirmed live).
BEZEL_TEST(ctor_out_var_da_2409,
           nullable_reference_unguarded_member_access_rejected_spec_7_4) {
    BEZEL_EXPECT_TRUE(hasMsg(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string?) "
        "    n := \"hi\"; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    return n.Length; "
        "  end Main; end C;",
        "on a nullable value requires narrowing"));
}

// Control/value pairing for the two reference-type reject tests above:
// once genuinely typed `string?`, the ordinary §7.4 narrowing pattern
// (`if n <> null then ... end;`) must still ADMIT the guarded access and
// return the real value — proving the corrected type keeps functioning for
// the legitimate pattern, not merely rejecting the unsound one. (This does
// NOT itself distinguish pre/post-fix at the value level — re-verified live
// that it runs identically either way, since codegen's null-check on a
// reference type never consulted the sema TypeRef — so its role here is a
// control, not a mechanical proof; the two reject tests above carry that.)
BEZEL_TEST(ctor_out_var_da_2409,
           nullable_reference_guarded_member_access_accepted_real_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string?) "
        "    n := \"hi\"; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    if n <> null then return n!.Length; end; "
        "    return -1; "
        "  end Main; end C;"), 2);
}

// Control: a NON-nullable reference ctor out-var (`out n: string`, no `?`)
// keeps inferring plain `string` — unaffected by removing the strip (its
// declType already has `nullable == false`, so the strip was always a
// no-op for this case). An unguarded member access stays ACCEPTED.
BEZEL_TEST(ctor_out_var_da_2409,
           nonnullable_reference_out_var_unaffected_by_nullable_fix) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := \"hi\"; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    return n.Length; "
        "  end Main; end C;"), 2);
}

// =============================================================================
// `out owned var` sibling form: the ctor registration's second block (owned
// out-vars) got the same no-strip fix. A nullable `out owned T?` recipient
// must likewise keep its nullable type — unguarded member access rejected.
//
// NOTE on #2411 (adjacent, unrelated, NOT fixed here): `new T(out owned var
// x)` ICEs unconditionally once it reaches CODEGEN ("lowerExpr returned
// null for call argument in buildOrderedArgs — #1386"), confirmed
// unaffected by this fix (reproduces identically pre- and post-fix via the
// same patch-revert procedure this file's header describes). This
// particular test shape never reaches codegen at all: the UNGUARDED
// `c.Use()` this test asserts on is itself the §7.4 violation under test,
// so — once the recipient's type is correctly `Cn?` (post-fix) — sema
// rejects the program before codegen runs, sidestepping #2411 entirely.
// Pre-fix (verified via the same revert procedure), sema saw a
// stripped-to-non-nullable `c` and raised NO error for the unguarded
// access, so the program proceeded to codegen and hit #2411's ICE instead
// — a DIFFERENT diagnostic, so `hasMsg`'s §7.4-text search still correctly
// observes "not found" pre-fix (failing the assertion, as required) and
// "found" post-fix, without this test asserting anything about #2411 itself.
// =============================================================================

BEZEL_TEST(ctor_out_var_da_2409,
           out_owned_var_nullable_unguarded_member_access_rejected_spec_7_4) {
    BEZEL_EXPECT_TRUE(hasMsg(
        "module M; class Cn public method Use(): int => 1; end Cn; "
        "class Pool "
        "  public constructor(out owned conn: Cn?) "
        "    conn := new Cn(); "
        "  end; "
        "end Pool; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new Pool(out var c); "
        "    _ := p; "
        "    return c.Use(); "
        "  end Main; end C;",
        "on a nullable value requires narrowing"));
}
