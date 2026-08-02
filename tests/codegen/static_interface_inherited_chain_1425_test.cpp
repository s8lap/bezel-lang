// Issue #1425 sub-item (1): "inherited static-interface chains" — a follow-up
// deliberately out of scope for the v1 §11.8 landing (f7cce95, #807): "checkStaticInterfaceConformance
// walks a type's DIRECT base-list interfaces only. An interface EXTENDING a
// static-bearing interface (`interface IRich<T>: INumber<T>`), with a class
// conforming via the derived interface, does not yet propagate the base's
// static obligations (nor does `T.Member` search transitive constraint-
// interface bases)." Authority: spec §11.8 (v0.228) "Dispatch through the
// type parameter" / "The recurrence constraint"; stdlib §2.19 `INumber<T>`.
//
// TWO independently-observed bugs, both fixed here:
//
//  (a) FALSE-ACCEPT (conformance check). `Resolver::checkStaticInterfaceConformance`
//      (src/sema/TypeResolution_Checks.cpp) walked only `t->baseTypes` — a
//      class's OWN direct interface list — and read statics from `iface->members`
//      alone. A class conforming via a DERIVED interface (`Meters: IRich<Meters>`
//      where `IRich<T>: INumber<T>`) was never checked against INumber's
//      inherited statics at all: `class Meters: IRich<Meters>` with ZERO of
//      Zero/One/+/-/*//< implemented compiled CLEAN (confirmed before this fix).
//      Fixed by a recursive walk (checkIfaceStatics) over each direct
//      interface's OWN `baseTypeRefs`/`baseTypes` (populated for an
//      interface's `: Base<...>` list exactly like a class's), composing the
//      generic-argument substitution at each hop.
//
//  (b) FALSE-REJECT (T-typed operator dispatch). `ExprTC::findConstraintStaticOperator`
//      (src/sema/ExprTypecheck.cpp) scanned only the DIRECT constraint bound's
//      `iface->members` for a matching `static operator`. `T.Zero()` (a
//      METHOD) already searched transitively via `lookupMember`'s baseTypes
//      BFS and worked; a BINARY OPERATOR on T-typed operands (`total + x`)
//      did not, and was rejected: "operator `+` is not defined for type `?`"
//      — even though the constraint's ANCESTOR interface (INumber, reached
//      only through IRich) declares it. Fixed by the same BFS-over-baseTypes
//      shape lookupMember already uses, applied inside findConstraintStaticOperator.
//
// Both bugs were confirmed via `bezelc -c` on hand-written probes before
// writing this test (VERIFY-FIRST, per CLAUDE.md): (a) a zero-static Meters
// conforming only via IRich compiled with NO diagnostic; (b) the spec's own
// Sum<T> shape, T constrained by IRich<T> instead of INumber<T> directly,
// rejected `total + x` while `T.Zero()` on the same line compiled fine.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool rejects(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// IRich<T> extends the stdlib INumber<T> (§2.19) WITHOUT redeclaring any of
// its statics — the shape #1425 sub-item (1) names.
const char* kIRich =
    "public interface IRich<T>: INumber<T> where T: IRich<T> "
    "end IRich; ";

// A full conformer of IRich<Meters> (== INumber<Meters>'s obligations,
// inherited). Producer markers per stdlib §2.19 v0.196.
const char* kMetersViaIRich =
    "public class Meters: IRich<Meters> "
    "  public Value: int; "
    "  public constructor(v: int) this.Value := v; end; "
    "  public static method Zero(): owned Meters => new Meters(0); "
    "  public static method One(): owned Meters => new Meters(1); "
    "  public static method operator +(a: Meters, b: Meters): owned Meters => "
    "    new Meters(a.Value + b.Value); "
    "  public static method operator -(a: Meters, b: Meters): owned Meters => "
    "    new Meters(a.Value - b.Value); "
    "  public static method operator *(a: Meters, b: Meters): owned Meters => "
    "    new Meters(a.Value * b.Value); "
    "  public static method operator /(a: Meters, b: Meters): owned Meters => "
    "    new Meters(a.Value / b.Value); "
    "  public static method operator <(a: Meters, b: Meters): bool => "
    "    a.Value < b.Value; "
    "  public static method operator >(a: Meters, b: Meters): bool => "
    "    a.Value > b.Value; "
    "  public static method operator <=(a: Meters, b: Meters): bool => "
    "    a.Value <= b.Value; "
    "  public static method operator >=(a: Meters, b: Meters): bool => "
    "    a.Value >= b.Value; "
    "end Meters; ";

// The spec §11.8 Sum<T> example VERBATIM, constrained by the DERIVED
// interface IRich<T> rather than INumber<T> directly.
const char* kSumViaIRich =
    "static class F "
    "  public static method Sum<T>(items: Array<T>): owned T where T: IRich<T> "
    "    var total := T.Zero(); "
    "    for var x in items do total := total + x; end; "
    "    return total; "
    "  end Sum; "
    "end F; ";

}  // namespace

// (a) FALSE-ACCEPT fixed: a class conforming via a DERIVED interface with
// NONE of the ancestor's inherited statics implemented is now REJECTED,
// naming every missing member (Zero/One/+/-/*//<).
BEZEL_TEST(static_iface_inherited_chain_1425,
           empty_conformer_via_derived_interface_rejected_spec_11_8) {
    bool r = rejects(
        std::string("module M; ") + kIRich +
        "public class Meters: IRich<Meters> "
        "  public Value: int; "
        "  public constructor(v: int) this.Value := v; end; "
        "end Meters; "
        "class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(r);
}

// (a) control: a FULL conformer via the derived interface is accepted (no
// false-reject introduced by the transitivity fix).
BEZEL_TEST(static_iface_inherited_chain_1425,
           full_conformer_via_derived_interface_accepted_spec_11_8) {
    auto res = bezel::test::runAot(
        std::string("module M; ") + kIRich + kMetersViaIRich +
        "class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(res.ran);
    BEZEL_EXPECT_EQ(res.exitCode, 0);
}

// (b) FALSE-REJECT fixed: `T.Zero()` (already worked) AND `total + x`
// (previously rejected: "operator `+` is not defined for type `?`") both
// dispatch through the inherited chain — Sum([10,13,19]) = 42.
BEZEL_TEST(static_iface_inherited_chain_1425,
           sum_dispatches_operator_through_inherited_chain_spec_11_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIRich + kMetersViaIRich + kSumViaIRich +
        "class C public static method Main(): int "
        "  using var m0 := new Meters(10); using var m1 := new Meters(13); "
        "  using var m2 := new Meters(19); "
        "  using var xs := new Array<Meters>(); "
        "  xs.Add(m0); xs.Add(m1); xs.Add(m2); "
        "  var t := F.Sum(xs); "
        "  return t.Value; "
        "end Main; end C;"), 42);
}

// (b) isolation control: `T.Zero()` ALONE (no operator) through the
// inherited chain — pins that the method-dispatch half (already working via
// lookupMember's transitive baseTypes BFS) is unaffected by this fix.
BEZEL_TEST(static_iface_inherited_chain_1425,
           method_only_dispatch_through_inherited_chain_spec_11_8) {
    const char* kMakeZero =
        "static class F "
        "  public static method MakeZero<T>(): owned T where T: IRich<T> "
        "    return T.Zero(); "
        "  end MakeZero; "
        "end F; ";
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIRich + kMetersViaIRich + kMakeZero +
        "class C public static method Main(): int "
        "  using var z := F.MakeZero<Meters>(); "
        "  return z.Value; "
        "end Main; end C;"), 0);
}
