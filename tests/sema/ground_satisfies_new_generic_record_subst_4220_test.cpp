// Spec-derived tests for issue #4220: `ExprTC::groundSatisfiesNew`'s Record arm
// (src/sema/ExprTypecheck.cpp) never substituted a record's own generic
// parameter against the ground leaf's `genericArgs` at all — the sibling of
// #4002's bug (already fixed for `Resolver::argSatisfiesNew`, TypeResolution_
// Checks.cpp), never ported to this `groundSatisfiesNew` (§19.6 v0.196
// `case`-exhaustiveness) leaf test.
//
// §14.2 "Constraints": a record satisfies `where T: new()` iff `default(R)`
// is well-defined, i.e. every field (after generic substitution) has a
// well-defined default. §17.11 "The default operator": "Not defaultable:
// non-nullable reference types". §19.6 (v0.196): a ground leaf solution that
// PROVABLY violates a scalar `where`-constraint (here, `new()`) is not a
// well-formed instantiation, so the leaf is EXEMPT from exhaustiveness
// coverage; a solution that satisfies (or an undecidable one) keeps the leaf
// REQUIRED.
//
// Old (buggy) behavior: `storageComponentType(pf, ctx_)` returned the bare,
// declaration-less field type AS WRITTEN (e.g. `T` for `record Box<T>(V:
// T)`), never substituted against `arg->genericArgs`; the local `nullable`/
// `groundCategorize` leaf test classifies a bare declaration-less Named type
// as `GroundCat::Unknown`, never `Reference`, so the per-field loop never
// tripped regardless of what the leaf's ground solution actually bound `T`
// to. Every record with a bare-parameter field (directly, or nested inside a
// delegating record) satisfied `new()` unconditionally — so a leaf that
// should have been recognized as UNINHABITED (and therefore exempt, no arm
// needed) was instead wrongly kept REQUIRED, spuriously demanding an arm for
// a statically-unattainable instantiation.
//
// This file exercises BOTH directions per §19.6's own soundness requirement:
// a leaf that IS statically unattainable (must be exempted — no warning) and
// one that is NOT (must stay required — warning fires, naming the leaf). The
// fix ports the SAME `common::RecordSubstEnv`/`seedRecordSubstEnv`/
// `resolveRecordFieldSubst` composition, and the SAME shared per-field
// authority `sema::typeHasNoDefault` (TypeResolution.hpp), that fix #4166's
// review follow-up already unified `Resolver::argSatisfiesNew`'s sibling
// Record arm onto — rather than minting a fourth spelling (#4229).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::parseAndTypecheckGated;

// True iff some diagnostic mentions non-exhaustiveness at Warning severity —
// mirrors case_exhaustiveness_test.cpp's own helper (duplicated here rather
// than shared: that file's helpers are TU-local, in an anonymous namespace).
bool hasNonExhaustiveWarning(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool nonExhaustiveWarningMentions(const bezel::test::SemaResult& r,
                                  std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos &&
            d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Shared prelude: a non-nullable-reference class (no default), a generic
// record with a bare positional field, a delegating generic record wrapping
// it, and a record whose OWN generic parameter itself carries a `where`
// constraint (proving the record's own constraint doesn't confuse the
// `seedRecordSubstEnv` param-name -> arg mapping). `BGSN<T>` is the closed
// base hierarchy every row below matches over.
const char* kPrelude4220 =
    "module M4220; "
    "public class WidgetGSN "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "end WidgetGSN; "
    "public record RecNewLeaf4220<T>(V: T); "
    "public record RecOuterND4220<T>(In: RecNewLeaf4220<T>); "
    "public record RecConstrainedField4220<T>(V: T) where T: class; "
    "closed class BGSN4220<T> end BGSN4220; ";

} // namespace

// (1) CORE ROW — bare generic-parameter field, non-nullable class binding.
// `RecNewLeaf4220<WidgetGSN>`'s field `V: WidgetGSN` is a non-nullable
// reference type once substituted -> no default -> does NOT satisfy
// `where Y: new()` -> L4220 is UNINHABITED at this leaf -> EXEMPT. Covering
// only the ground leaf `RGSN4220` is therefore fully exhaustive: NO warning.
//
// FAILS WITHOUT THE FIX: the old code never substitutes `T`, so `V`'s bare,
// declaration-less type classifies `GroundCat::Unknown` (never `Reference`),
// the loop never trips, and the Record arm unconditionally returns satisfied
// -> L4220 wrongly stays REQUIRED -> the warning wrongly fires.
BEZEL_TEST(sema_ground_satisfies_new_4220,
           bare_generic_param_nonnullable_class_leaf_exempt_spec_19_6) {
    std::string src =
        std::string(kPrelude4220) +
        "sealed class LGSN4220<Y> : BGSN4220<Y> where Y: new() end LGSN4220; "
        "sealed class RGSN4220 : BGSN4220<RecNewLeaf4220<WidgetGSN>> "
        "end RGSN4220; "
        "class CalcGSN4220a "
        "  public method M(b: BGSN4220<RecNewLeaf4220<WidgetGSN>>): int "
        "    var a := (case b of "
        "      var r: RGSN4220 => 1 "
        "); "
        "    return a; "
        "  end M; "
        "end CalcGSN4220a;";
    auto r = parseAndTypecheckGated(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // LGSN4220 exempt
}

// (2) THE COMPANION — same hierarchy, `Y := RecNewLeaf4220<int>`. `int` IS
// defaultable, so `V: int` has a well-defined default -> `RecNewLeaf4220<int>`
// satisfies `new()` -> LGSN4220 is DETERMINED and INHABITED -> the warning
// MUST still fire, naming LGSN4220 (§19.6's own guard: the filter may only
// drop a PROVABLY-violating solution, never a satisfying one).
//
// Passes identically before and after the fix (an accept-side control): the
// pre-fix code returns satisfied=true here too, but for the WRONG reason (it
// never inspects `T` at all) — this row's purpose is only to prove the fix
// does not flip this direction, not to distinguish pre/post fix.
BEZEL_TEST(sema_ground_satisfies_new_4220,
           bare_generic_param_value_type_leaf_stays_required_spec_19_6) {
    std::string src =
        std::string(kPrelude4220) +
        "sealed class LGSN4220b<Y> : BGSN4220<Y> where Y: new() end LGSN4220b; "
        "sealed class RGSN4220b : BGSN4220<RecNewLeaf4220<int>> "
        "end RGSN4220b; "
        "class CalcGSN4220b "
        "  public method M(b: BGSN4220<RecNewLeaf4220<int>>): int "
        "    var a := (case b of "
        "      var r: RGSN4220b => 1 "
        "); "
        "    return a; "
        "  end M; "
        "end CalcGSN4220b;";
    auto r = parseAndTypecheckGated(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "LGSN4220b"));
}

// (3) NESTED / DELEGATING RECORD — `Y := RecOuterND4220<WidgetGSN>`. The
// bare-field substitution must recurse through a delegating record's OWN
// field (`In: RecNewLeaf4220<T>`), not just a one-level bare parameter.
// `In`, substituted, is `RecNewLeaf4220<WidgetGSN>`; `typeHasNoDefault`
// recurses into ITS field `V: WidgetGSN` (non-nullable class) -> no default
// at any depth -> `RecOuterND4220<WidgetGSN>` does not satisfy `new()` ->
// LGSN4220c is EXEMPT -> NO warning.
//
// FAILS WITHOUT THE FIX for the identical reason as row (1): the loop never
// substitutes `In`'s bare declared type either, at any nesting depth.
BEZEL_TEST(sema_ground_satisfies_new_4220,
           delegating_record_nonnullable_class_leaf_exempt_spec_19_6) {
    std::string src =
        std::string(kPrelude4220) +
        "sealed class LGSN4220c<Y> : BGSN4220<Y> where Y: new() end LGSN4220c; "
        "sealed class RGSN4220c : BGSN4220<RecOuterND4220<WidgetGSN>> "
        "end RGSN4220c; "
        "class CalcGSN4220c "
        "  public method M(b: BGSN4220<RecOuterND4220<WidgetGSN>>): int "
        "    var a := (case b of "
        "      var r: RGSN4220c => 1 "
        "); "
        "    return a; "
        "  end M; "
        "end CalcGSN4220c;";
    auto r = parseAndTypecheckGated(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // LGSN4220c exempt
}

// (4) RECORD WHOSE OWN PARAMETER IS ITSELF CONSTRAINED — `Y :=
// RecConstrainedField4220<WidgetGSN>`. `RecConstrainedField4220<T>` declares
// `where T: class`; `WidgetGSN` (a non-nullable class) satisfies THAT
// constraint, so the instantiation itself is well-formed, but the record's
// OWN field `V: WidgetGSN` is still a non-nullable reference field with no
// default. Proves the record's own `where` clause on its generic parameter
// does not confuse `seedRecordSubstEnv`'s param-name -> arg mapping (the
// substitution env is keyed by NAME, populated from `d->genericParams`
// regardless of what constraints those params carry).
//
// FAILS WITHOUT THE FIX for the identical reason as row (1).
BEZEL_TEST(
    sema_ground_satisfies_new_4220,
    record_with_own_constrained_param_nonnullable_class_leaf_exempt_spec_19_6) {
    std::string src =
        std::string(kPrelude4220) +
        "sealed class LGSN4220d<Y> : BGSN4220<Y> where Y: new() end LGSN4220d; "
        "sealed class RGSN4220d : BGSN4220<RecConstrainedField4220<WidgetGSN>> "
        "end RGSN4220d; "
        "class CalcGSN4220d "
        "  public method M(b: BGSN4220<RecConstrainedField4220<WidgetGSN>>): "
        "int "
        "    var a := (case b of "
        "      var r: RGSN4220d => 1 "
        "); "
        "    return a; "
        "  end M; "
        "end CalcGSN4220d;";
    auto r = parseAndTypecheckGated(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // LGSN4220d exempt
}

// (5) POSITIVE CONTROL — `Array<int>` (stdlib §4.4, a builtin class with a
// public parameterless constructor) bound DIRECTLY (not nested in a record
// field) at the TOP level of `where Y: new()`. Array's TypeRef carries a
// NULL `declaration` (TypeResolution_Names.cpp's null-declaration builtin
// representation — the same shape as ~50 other always-available stdlib
// types), so it never reaches this function's Record arm at all: it falls
// straight through every `arg->declaration`-gated branch to the final
// "Built-ins / generic-param / function args: defer" catch-all
// (`decided` stays false). Undecided is treated as "keep" by the caller
// (`leafSolutionViolatesScalarConstraint`'s `violates` lambda: `decided &&
// !ok` is false when `decided` is false), so LArr4220 stays REQUIRED
// regardless — unaffected by this fix, which touches only the Record arm.
// Unchanged before and after the fix; guards against the fix's Record-arm
// change ever regressing a control neither this issue nor #4220's Record
// arm has any business touching.
BEZEL_TEST(sema_ground_satisfies_new_4220,
           builtin_array_top_level_leaf_stays_required_spec_19_6) {
    std::string src =
        std::string(kPrelude4220) +
        "sealed class LArr4220<Y> : BGSN4220<Y> where Y: new() end LArr4220; "
        "sealed class RArr4220 : BGSN4220<Array<int>> end RArr4220; "
        "class CalcArr4220 "
        "  public method M(b: BGSN4220<Array<int>>): int "
        "    var a := (case b of "
        "      var r: RArr4220 => 1 "
        "); "
        "    return a; "
        "  end M; "
        "end CalcArr4220;";
    auto r = parseAndTypecheckGated(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "LArr4220"));
}
