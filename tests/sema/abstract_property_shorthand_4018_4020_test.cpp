// Spec-derived tests for fix #4018 / #4020 — §12.5 "Abstract classes and
// members" (v0.294) — two halves of ONE landmine plus two adjacent gaps
// found auditing the same hazard.
//
// `abstract property Name: T;` (§12.5, the "abstract shorthand") parses into
// an ast::PropertyDecl with `bodyKind = PropertyBodyKind::Auto` — the SAME
// value an ordinary auto-property with real backing storage gets
// (Parser_Decls.cpp, the class-member property body dispatch). The
// `abstract` modifier is the only AST signal distinguishing "no backing
// storage, subclasses must implement" from "real storage", and multiple
// independent sites asked this question with their own hand-rolled modifier
// scan, drifting out of step with each other (LANE_RULES §6). This fix adds
// one shared predicate, `sema::isAbstractPropertyShorthand` (sema/Symbol.hpp
// / SymbolCollection.cpp), and points every site that needs the answer at
// it:
//
//   - DefiniteAssignment.cpp's isSevenNineStrictAutoProp (#4018: FALSE
//     REJECT — an abstract class's own constructor was rejected over an
//     unrelated abstract property it can never need to initialize).
//   - TypeResolution_Checks.cpp's per-type abstract-member BFS (#4020:
//     SIGSEGV — a concrete class's self-declared, unoverridden abstract
//     property was silently accepted as satisfying its own contract; the
//     named-property sibling of #3641's already-fixed indexer gap).
//   - TypeResolution_Checks.cpp's `pure`-on-auto-property redundancy check
//     (found auditing every PropertyBodyKind::Auto reader for the same
//     hazard: `pure abstract property` was false-rejected as "redundant"
//     with the identical reasoning the check's own comment already carves
//     out for INTERFACE properties, just never extended to a class's own
//     abstract shorthand).
//   - Ownership_Checks.cpp's checkAutoPropertyInitializer (#2168's owned-
//     initializer screen: `abstract property Name: T := new Owned();` was
//     false-rejected as "leaking into a non-owned backing field" that was
//     never generated in the first place).
//
// Because the pre-fix #4020 symptom is a SIGSEGV rather than a clean
// diagnostic-free compile, its reject-arm test uses
// failedAtCompileTimeWithRealLocation (aot_result_asserts.hpp) rather than a
// bare `!r.ran`, which a crash/signal would satisfy too.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// #4018 — DefiniteAssignment.cpp's isSevenNineStrictAutoProp / runConstructor
// false-rejects an abstract class's own constructor over an unrelated
// abstract property.
// =====================================================================

// The issue's own repro: an abstract class with an EXPLICIT constructor
// (checking an unrelated field is not needed to trigger this — merely having
// one at all is enough, since runConstructor's field-gathering loop scans
// EVERY property on the declaring class regardless of what the ctor body
// does) plus a class-level `abstract property`, and a concrete subclass
// overriding it. Must compile AND run, proving the override's value is used
// (not merely "compiles").
BEZEL_TEST(sema_abstract_property_shorthand_4018_4020,
           abstract_class_ctor_with_abstract_property_accepted_spec_12_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class BaseAP4018 "
        "  public abstract property Name: string; "
        "  public constructor() end constructor; "
        "end BaseAP4018; "
        "class DAP4018 : BaseAP4018 "
        "  public override property Name: string => \"derived\"; "
        "end DAP4018; "
        "class C "
        "  public static method Main(): int "
        "    var d := new DAP4018(); "
        "    return d.Name.Length; "
        "  end Main; "
        "end C;"),
        7);
}

// Control: the fix must not disable §7.9 for an ORDINARY (non-abstract)
// auto-property on the same abstract class — a real omission must still be
// rejected by the exact per-constructor completion screen #4018 shares a
// file with.
BEZEL_TEST(sema_abstract_property_shorthand_4018_4020,
           abstract_class_ctor_omits_real_field_still_rejected_control_spec_7_9) {
    auto r = runAot(
        "module M; "
        "public abstract class BaseAP4018Ctrl "
        "  public abstract property Name: string; "
        "  public property Extra: string; "
        "  public constructor() end constructor; "
        "end BaseAP4018Ctrl; "
        "class DAP4018Ctrl : BaseAP4018Ctrl "
        "  public override property Name: string => \"d\"; "
        "end DAP4018Ctrl; "
        "class C "
        "  public static method Main(): int "
        "    var d := new DAP4018Ctrl(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "constructor does not initialize non-nullable field "
                        "`Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// #4020 — TypeResolution_Checks.cpp's per-type BFS never records a
// class-declared abstract NAMED property as an obligation, so a concrete
// class satisfies its own unimplemented contract. §23.4 states an indexer
// "participate[s] in virtual/override/sealed/abstract like methods" — the
// already-fixed #3641 sibling; nothing carved named properties out
// differently, but the implementation had.
// =====================================================================

// The issue's own repro: a plain (non-abstract, non-closed) class declaring
// a named `abstract property` with no override. Before the fix this compiled
// clean and SIGSEGV'd on first read; must now be a compile-time rejection
// with a real, pinned location — a bare `!r.ran` cannot distinguish this
// from the pre-fix crash.
BEZEL_TEST(sema_abstract_property_shorthand_4018_4020,
           concrete_class_unimplemented_abstract_property_rejected_spec_12_5) {
    auto r = runAot(
        "module M; "
        "class Base4020 "
        "  public abstract property Name: string; "
        "end Base4020; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Base4020(); "
        "    return b.Name.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "concrete class `Base4020` does not implement "
                        "inherited abstract member `Name`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
}

// The control paired with the repro above — the identical shape with
// `abstract method Area(): int;` in place of the property was ALREADY
// correctly rejected before this fix (the issue's own measured control,
// confirming the property path specifically was the gap, not general
// abstract-member handling). Pinned here so a future regression in the
// shared BFS surfaces on both members, not just the property one.
BEZEL_TEST(sema_abstract_property_shorthand_4018_4020,
           concrete_class_unimplemented_abstract_method_already_rejected_control_spec_12_5) {
    auto r = runAot(
        "module M; "
        "class Base4020Method "
        "  public abstract method Area(): int; "
        "end Base4020Method; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Base4020Method(); "
        "    return b.Area(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "concrete class `Base4020Method` does not implement "
                        "inherited abstract member `Area`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
}

// ACCEPT control: a genuine override on the DECLARING class itself (not just
// a subclass, per #4018's own shape) must still compile and run correctly —
// proves the new obligation-tracking does not false-reject the ordinary
// idiom pinned by
// tests/lit/sema/sema_prop_override_1614__abstract_shorthand_override_ok.bzl.
BEZEL_TEST(sema_abstract_property_shorthand_4018_4020,
           concrete_class_implemented_abstract_property_accepted_spec_12_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "abstract class Base4020Ok "
        "  public abstract property Name: string; "
        "end Base4020Ok; "
        "class D4020Ok : Base4020Ok "
        "  public override property Name: string => \"impl\"; "
        "end D4020Ok; "
        "class C "
        "  public static method Main(): int "
        "    var d: Base4020Ok := new D4020Ok(); "
        "    return d.Name.Length; "
        "  end Main; "
        "end C;"),
        4);
}

// ACCEPT control: the obligation is satisfied by a class TWO LEVELS up the
// chain (mirrors sema_abs_impl__abstract_class_indexer_satisfied_two_levels_
// up_ok.bzl's indexer analogue) — proves the BFS still walks the full
// ancestor chain for the new abstractPropertyNames obligation, not just the
// immediate base.
BEZEL_TEST(sema_abstract_property_shorthand_4018_4020,
           abstract_property_satisfied_two_levels_up_accepted_spec_12_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "abstract class Root4020 "
        "  public abstract property Name: string; "
        "end Root4020; "
        "class Mid4020 : Root4020 "
        "  public override property Name: string => \"mid\"; "
        "end Mid4020; "
        "class Leaf4020 : Mid4020 "
        "end Leaf4020; "
        "class C "
        "  public static method Main(): int "
        "    var l := new Leaf4020(); "
        "    return l.Name.Length; "
        "  end Main; "
        "end C;"),
        3);
}

// =====================================================================
// Follow-on gap 1 — TypeResolution_Checks.cpp's `pure`-on-auto-property
// redundancy check already excludes INTERFACE properties from its
// "redundant" verdict (its own comment names the exact bodyKind::Auto
// ambiguity this whole issue is about) but had not been extended to a
// CLASS's own abstract shorthand.
// =====================================================================

BEZEL_TEST(sema_abstract_property_shorthand_4018_4020,
           abstract_pure_property_accepted_spec_12_5_22_15) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class BasePure4018 "
        "  public pure abstract property Name: string; "
        "end BasePure4018; "
        "class DPure4018 : BasePure4018 "
        "  public pure override property Name: string => \"pure\"; "
        "end DPure4018; "
        "class C "
        "  public static method Main(): int "
        "    var d := new DPure4018(); "
        "    return d.Name.Length; "
        "  end Main; "
        "end C;"),
        4);
}

// Control: `pure` on an ORDINARY (non-abstract) class auto-property must
// still be rejected as redundant — the fix narrows the exclusion to the
// abstract shorthand specifically, it does not disable the check.
BEZEL_TEST(sema_abstract_property_shorthand_4018_4020,
           pure_on_ordinary_autoprop_still_rejected_control_spec_22_15) {
    auto r = runAot(
        "module M; "
        "public class HolderPureCtrl "
        "  public pure property Name: string; "
        "end HolderPureCtrl; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "`pure` on an auto-property is redundant"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.15"));
}

// =====================================================================
// Follow-on gap 2 — Ownership_Checks.cpp's checkAutoPropertyInitializer
// (#2168's owned-initializer screen) reasons about "the generated backing
// field", which an abstract shorthand property never gets.
// =====================================================================

// fix #4026 (§12.5, v0.295) updated this test's expectation: the shape below
// no longer compiles clean. #4026 added a dedicated rejection for an
// abstract-shorthand property's initializer (it has no backing storage to
// write into, so the expression could never run) — TypeResolution_Checks.cpp
// runs before Ownership_Checks.cpp's `checkAutoPropertyInitializer`, so this
// program is now rejected for the NEW §12.5 reason. Renamed from
// `..._accepted_...` (its pre-#4026 name) to make that reversal visible.
// The assertion that #2168's ownership check does NOT ALSO fire is
// preserved — this program must be rejected for the #4026 dead-initializer
// reason, not re-acquire the WRONG "leaks into a non-owned backing field"
// diagnostic this test file's own #4018/#4020 fix stopped from firing on
// this exact shape.
BEZEL_TEST(sema_abstract_property_shorthand_4018_4020,
           abstract_property_owned_initializer_now_rejected_by_4026_spec_12_5) {
    auto r = runAot(
        "module M; "
        "class Widget4018 end Widget4018; "
        "public abstract class BaseInit4018 "
        "  public abstract property Current: Widget4018 := new Widget4018(); "
        "end BaseInit4018; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "may not declare an initializer"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
    BEZEL_EXPECT_FALSE(diagContains(
        r, "freshly-produced owned value cannot initialize"));
}

// Control: the identical owned-`new`-initializer shape on an ORDINARY
// (non-abstract) auto-property must still be rejected — #2168's pinned
// behavior, unaffected by this fix's narrower exclusion.
BEZEL_TEST(sema_abstract_property_shorthand_4018_4020,
           ordinary_autoprop_owned_initializer_still_rejected_control_spec_2168) {
    auto r = runAot(
        "module M; "
        "class Widget4018Ctrl end Widget4018Ctrl; "
        "class HolderInitCtrl "
        "  public readonly property Current: Widget4018Ctrl := "
        "      new Widget4018Ctrl(); "
        "end HolderInitCtrl; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "freshly-produced owned value cannot initialize "
                        "auto-property `Current`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}
