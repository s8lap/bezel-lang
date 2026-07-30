// Spec-derived regression test for issue #2416 — a RECORD satisfying a
// read-write interface property with a GETTER-ONLY implementation compiled
// clean and then SIGSEGVd on write through the interface reference. The
// identical defect shape #2413 fixed for CLASSES, but #2413's own fix
// (`Resolver::checkAbstractImplementations`, TypeResolution_Checks.cpp)
// filtered its outer loop to `TypeDeclKind::Class` only, so a record got
// NONE of that function's §11.2 conformance checks — not #489's "field
// doesn't satisfy a property" check, not §2380's property-type-match check,
// and not #2413's accessor-set check.
//
// §9.6 (v0.235): "A record may implement any number of interfaces" — records
// are bound by the identical §11.2 conformance obligations as classes; §9.6
// draws no distinction. This fix widens `checkAbstractImplementations`'s
// outer-loop filter to admit `TypeDeclKind::Record`.
//
// A RECORD-SPECIFIC wrinkle surfaced during this fix: a record's POSITIONAL
// field (§9.1) auto-generates a public READ-ONLY property of the same name
// ("Public read-only properties, one per positional field" — §9.1; "Properties
// auto-generated from positional fields are read-only" — §9.4), but is
// represented internally as a `FieldSymbol` (with `posParam` set), not a
// `PropertySymbol` (issue #815's own finding) — so it needed its own
// accessor-set / type-match handling alongside the pre-existing
// PropertySymbol-keyed checks (`concretePosFields` in
// checkAbstractImplementations). This file exercises BOTH shapes: an
// explicit getter-only property declaration (the direct #2413 analog) and a
// positional-field-backed satisfaction (the new record-specific shape).
//
// §11.2 (v0.235) diamond-resolution note: "A read-write member satisfies a
// read-only interface member of the same name and type" — by omission the
// converse does not hold. §23.3/§9.4 supply the accessor-set definitions the
// obligation and the implementation are each read against.
//
// See getter_only_readwrite_iface_property_2413_test.cpp for the
// `hasGetterOnlyVsReadWriteError`-shaped rationale on why `rejects()` alone
// is an insufficient pin (a runtime SIGSEGV synthesizes an Error diagnostic
// through `runAot`'s WIFSIGNALED path too) — this file follows the same
// discipline: every REJECT case asserts on specific diagnostic TEXT, not
// merely `rejects()`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

bool rejects(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// Any ERROR diagnostic containing every one of `needles` (order-independent
// substring match) — used to pin specific diagnostic wording per case below.
bool hasErrorContainingAll(const std::string& src,
                           std::initializer_list<const char*> needles) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        bool all = true;
        for (const char* n : needles) {
            if (d.message.find(n) == std::string::npos) { all = false; break; }
        }
        if (all) return true;
    }
    return false;
}

// ---- REJECT case 1 (#2416's own repro — explicit getter-only property) ---
// The issue's confirmed repro shape, verified against origin/main before
// this fix: compiles clean (exit 0), SIGSEGVs (signal 11) on `f.Val := 42`.
const char* kExplicitGetterOnlyRejected =
    "module M; "
    "public interface IHas "
    "  property Val: int; "
    "end IHas; "
    "public record R(X: int) : IHas "
    "  public property Val: int "
    "    get => do return X; end; "
    "  end Val; "
    "end R; "
    "public class C "
    "  public static method Main(): int "
    "    var r := new R(9); "
    "    var f: IHas := r; "
    "    f.Val := 42; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- REJECT case 2 (positional-field-backed satisfaction — §9.1) ---------
// A record's positional field of the same name as the interface property —
// no explicit property declaration at all. §9.1/§9.4: always getter-only.
const char* kPositionalFieldGetterOnlyRejected =
    "module M; "
    "public interface IHas "
    "  property Val: int; "
    "end IHas; "
    "public record R(Val: int) : IHas end R; "
    "public class C "
    "  public static method Main(): int "
    "    var r := new R(9); "
    "    var f: IHas := r; "
    "    f.Val := 42; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- REJECT case 3 (#489 sibling: a record field named like the property,
//      but NOT the positional-field shape — a plain body field never
//      satisfies a property contract, for a record exactly as for a class) -
// R declares an ordinary (non-positional) public field `Val`; §11.2/§23's
// "a field does not satisfy a property contract" rule applies identically.
// Also pins the record/class wording fix (kindWord): the diagnostic must say
// "record `R`", not "class `R`".
const char* kPlainFieldDoesNotSatisfyProperty =
    "module M; "
    "public interface IHas "
    "  readonly property Val: int; "
    "end IHas; "
    "public record R : IHas "
    "  public Val: int := 9; "
    "end R; "
    "public class C "
    "  public static method Main(): int "
    "    var r := new R(); "
    "    var f: IHas := r; "
    "    return f.Val; "
    "  end Main; "
    "end C;";

// ---- REJECT case 4 (§2380 sibling: property TYPE mismatch on a record) ---
// R's `Val` is a `string`-typed getter satisfying a `readonly` `int`-typed
// interface obligation — the accessor sets match (both getter-only), so this
// isolates the TYPE-match check specifically.
const char* kRecordPropertyTypeMismatch =
    "module M; "
    "public interface IHas "
    "  readonly property Val: int; "
    "end IHas; "
    "public record R(Tag: string) : IHas "
    "  public property Val: string => Tag; "
    "end R; "
    "public class C "
    "  public static method Main(): int "
    "    var r := new R(\"x\"); "
    "    var f: IHas := r; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 1 (explicit getter-only vs readonly — stays legal,
//      correct runtime value) ----------------------------------------------
const char* kExplicitGetterOnlyVsReadonlyAccepted =
    "module M; "
    "public interface IHas "
    "  readonly property Val: int; "
    "end IHas; "
    "public record R(X: int) : IHas "
    "  public property Val: int => X; "
    "end R; "
    "public class C "
    "  public static method Main(): int "
    "    var r := new R(42); "
    "    var f: IHas := r; "
    "    return f.Val; "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 2 (positional field vs readonly — stays legal,
//      correct runtime value; the #815 shape unaffected by #2416) ---------
const char* kPositionalFieldVsReadonlyAccepted =
    "module M; "
    "public interface IHas "
    "  readonly property Val: int; "
    "end IHas; "
    "public record R(Val: int) : IHas end R; "
    "public class C "
    "  public static method Main(): int "
    "    var r := new R(42); "
    "    var f: IHas := r; "
    "    return f.Val; "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 3 (read-write record property vs read-write interface
//      property — the itable setter slot is genuinely filled) -------------
const char* kRecordReadWriteVsReadWriteAccepted =
    "module M; "
    "public interface IHas "
    "  property Val: int; "
    "end IHas; "
    "public record R : IHas "
    "  private _v: int := 0; "
    "  public property Val: int "
    "    get => do return _v; end; "
    "    set => do _v := value; end; "
    "  end Val; "
    "end R; "
    "public class C "
    "  public static method Main(): int "
    "    var r := new R(); "
    "    var f: IHas := r; "
    "    f.Val := 42; "
    "    return f.Val; "
    "  end Main; "
    "end C;";

}  // namespace

// ---- FAIL-without / PASS-with: getter-only record property vs read-write
//      interface property is REJECTED with #2413's accessor-set diagnostic.
BEZEL_TEST(record_iface_property_conformance_2416,
           explicit_getter_only_vs_readwrite_rejected_9_6_11_2) {
    BEZEL_EXPECT_TRUE(rejects(kExplicitGetterOnlyRejected));
    BEZEL_EXPECT_TRUE(hasErrorContainingAll(
        kExplicitGetterOnlyRejected,
        {"11.2", "getter-only", "read-write", "setter"}));
}

// ---- FAIL-without / PASS-with: positional-field-backed record property vs
//      read-write interface property is REJECTED (the #2416-specific
//      accessor-set extension, distinct diagnostic wording naming §9.1).
BEZEL_TEST(record_iface_property_conformance_2416,
           positional_field_vs_readwrite_rejected_9_1_9_4_11_2) {
    BEZEL_EXPECT_TRUE(rejects(kPositionalFieldGetterOnlyRejected));
    BEZEL_EXPECT_TRUE(hasErrorContainingAll(
        kPositionalFieldGetterOnlyRejected,
        {"9.1", "positional field", "getter-only", "read-write"}));
}

// ---- FAIL-without / PASS-with: #489's "a field does not satisfy a property
//      contract" now applies to records too; wording says "record", not
//      "class" (the kindWord fix).
BEZEL_TEST(record_iface_property_conformance_2416,
           plain_field_does_not_satisfy_property_9_6_11_2_23) {
    BEZEL_EXPECT_TRUE(rejects(kPlainFieldDoesNotSatisfyProperty));
    BEZEL_EXPECT_TRUE(hasErrorContainingAll(
        kPlainFieldDoesNotSatisfyProperty,
        {"record `R`", "does not implement interface property",
         "field does not satisfy a property contract"}));
}

// ---- FAIL-without / PASS-with: §2380's property-type-match check now
//      applies to records too.
BEZEL_TEST(record_iface_property_conformance_2416,
           record_property_type_mismatch_11_2) {
    BEZEL_EXPECT_TRUE(rejects(kRecordPropertyTypeMismatch));
    BEZEL_EXPECT_TRUE(hasErrorContainingAll(
        kRecordPropertyTypeMismatch,
        {"implements interface `IHas`", "different type",
         "must match exactly"}));
}

// ---- ACCEPT controls (over-rejection safety — must NOT regress) ----------

BEZEL_TEST(record_iface_property_conformance_2416,
           explicit_getter_only_vs_readonly_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(42, kExplicitGetterOnlyVsReadonlyAccepted));
}

BEZEL_TEST(record_iface_property_conformance_2416,
           positional_field_vs_readonly_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(42, kPositionalFieldVsReadonlyAccepted));
}

BEZEL_TEST(record_iface_property_conformance_2416,
           record_readwrite_vs_readwrite_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(42, kRecordReadWriteVsReadWriteAccepted));
}
