// Spec-derived regression tests for issue #2059 — the §14.2 "component path"
// sendable-constraint diagnostic (follow-up to #2050). Spec §14.2 "Derived-
// property constraints" (v0.201) closes with: "A failed derived-property
// constraint is diagnosed at the instantiation site naming the constraint,
// the offending type argument, and the structural reason **as a component
// path** — §3.6 rule 4, sharpened: for `sendable`, the failing §30.14 rule
// and the field/element chain that violates it."
//
// Pre-#2059 both constraint-check sites — the type-instantiation site
// (Resolver::checkOneConstraint in TypeResolution_Checks.cpp) and the
// method-call site (checkMethodGenericConstraints in ExprTypecheck_Calls.cpp)
// — emitted a flat "type argument `X` does not satisfy the constraint `where
// T: sendable` (§14.2)" message that named the constraint + offending type
// but NOT the offending COMPONENT (the field/element that violates §30.14).
// This file pins the appended component-path fragment at BOTH sites.
//
// Authority: spec §14.2 "Derived-property constraints" (v0.201) + §3.6 rule 4
// (sharpened v0.201) + §30.14 (sendable-type rules — value-type / rule 1
// sealing / rule 2 owned-sendable / rule 3 no-function/Object/interface/
// non-owned-class). The shared walker lives in Sendability.cpp; both check
// sites feed it the failed type and append the fragment to the flat
// diagnostic.

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::SemaResult;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff a `where T: sendable` constraint-failure diagnostic carried a
// component-path fragment naming `component` (typically a field or element
// identifier). Matches the §3.6-rule-4 sharpening — the fragment is APPENDED
// after the §14.2 citation, separated by ": ".
bool sendableDiagHasComponent(const SemaResult& r,
                              std::string_view component) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not satisfy the constraint") !=
                std::string::npos &&
            d.message.find("sendable") != std::string::npos &&
            d.message.find(component) != std::string::npos)
            return true;
    return false;
}

// True iff a `where T: sendable` constraint-failure diagnostic names the
// cited §30.14 sub-rule (e.g. "rule 3", "rule 1", "value-type rule",
// "§9.7 borrow-bearing record"). This pins the second half of the §3.6-
// rule-4 sharpening: "the failing §30.14 rule and the field/element chain".
bool sendableDiagHasRule(const SemaResult& r, std::string_view ruleText) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not satisfy the constraint") !=
                std::string::npos &&
            d.message.find("sendable") != std::string::npos &&
            d.message.find(ruleText) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── TYPE-INSTANTIATION SITE (Resolver::checkOneConstraint) ──────────────────

// §14.2 / §30.14 / §3.6 rule 4: a sealed class with one non-`owned`
// class-typed field fails rule 3 ("no non-`owned` class-typed fields"). The
// instantiation-site diagnostic must name the offending FIELD `f` and the
// failing §30.14 rule (rule 3).
BEZEL_TEST(sendable_component_diag_2059,
           type_inst_sealed_class_non_owned_field_names_field_spec_30_14_rule3) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenData public Val: int := 0; destructor end destructor; "
        "end OpenData; "
        "sealed class C "
        "  public f: OpenData; "
        "  public constructor() f := new OpenData(); end constructor; "
        "  destructor end destructor; "
        "end C; "
        "public class Mailbox<T> where T: sendable "
        "  public constructor() end constructor; "
        "end Mailbox; "
        "class P public static method Main(): int "
        "  using var mb := new Mailbox<C>(); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(sendableDiagHasComponent(r, "field `f`"));
    BEZEL_EXPECT_TRUE(sendableDiagHasRule(r, "rule 3"));
}

// §14.2 / §30.14 / §3.6 rule 4: a body record with one class-typed field is
// borrow-bearing per §9.7 (records hold only borrows; §9.7 forbids owned
// record fields). The instantiation-site diagnostic must name the offending
// FIELD `B` and cite the §9.7 borrow-bearing-record rule.
BEZEL_TEST(sendable_component_diag_2059,
           type_inst_record_class_field_names_field_spec_9_7) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenData public Val: int := 0; destructor end destructor; "
        "end OpenData; "
        "record R "
        "  public B: OpenData; "
        "end R; "
        "public class Mailbox<T> where T: sendable "
        "  public constructor() end constructor; "
        "end Mailbox; "
        "class P public static method Main(): int "
        "  using var mb := new Mailbox<R>(); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(sendableDiagHasComponent(r, "field `B`"));
    BEZEL_EXPECT_TRUE(sendableDiagHasRule(r, "borrow-bearing"));
}

// §14.2 / §30.14 / §3.6 rule 4: an OPEN user class is not transfer-closed —
// rule 1 (sealing) fails — and the structural reason is the class itself,
// not a field. The diagnostic must name rule 1.
//
// v0.242 (#3052): rule 1 has two arms — "The class is `sealed` (§12.4); or it
// is `closed` and every class in its module-closed hierarchy is itself
// transfer-closed" — so the message names both spellings. `OpenData` carries
// neither, which is what makes it the rule-1 case.
BEZEL_TEST(sendable_component_diag_2059,
           type_inst_open_class_names_sealing_rule_1_spec_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenData public Val: int := 0; destructor end destructor; "
        "end OpenData; "
        "public class Mailbox<T> where T: sendable "
        "  public constructor() end constructor; "
        "end Mailbox; "
        "class P public static method Main(): int "
        "  using var mb := new Mailbox<OpenData>(); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(sendableDiagHasRule(r, "neither `sealed` nor `closed`"));
    BEZEL_EXPECT_TRUE(sendableDiagHasRule(r, "rule 1"));
}

// §14.2 / §30.14 / §3.6 rule 4: `Object` is the one primitive whose runtime
// type is unverifiable, so it fails the value-type rule. The diagnostic must
// name `Object` and the value-type rule.
BEZEL_TEST(sendable_component_diag_2059,
           type_inst_object_arg_names_value_rule_spec_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "public class Mailbox<T> where T: sendable "
        "  public constructor() end constructor; "
        "end Mailbox; "
        "class P public static method Main(): int "
        "  using var mb := new Mailbox<Object>(); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(sendableDiagHasComponent(r, "`Object`"));
    BEZEL_EXPECT_TRUE(sendableDiagHasRule(r, "value-type rule"));
}

// ── METHOD-CALL SITE (checkMethodGenericConstraints) ────────────────────────

// §14.2 / §30.14 / §3.6 rule 4 (method-call site): the same shared walker
// feeds the method-call-site diagnostic. A sealed class with a non-owned
// class field names field `f` + rule 3 — exactly as the instantiation site
// does (#2059: "the component-path machinery is shared by both check
// sites").
BEZEL_TEST(sendable_component_diag_2059,
           method_call_sealed_class_non_owned_field_names_field_spec_30_14_rule3) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenData public Val: int := 0; destructor end destructor; "
        "end OpenData; "
        "sealed class C "
        "  public f: OpenData; "
        "  public constructor() f := new OpenData(); end constructor; "
        "  destructor end destructor; "
        "end C; "
        "static class Box "
        "  public static method Hold<T>(x: T): int where T: sendable => 1; "
        "end Box; "
        "class P public static method Main(): int "
        "  return Box.Hold(new C()); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(sendableDiagHasComponent(r, "field `f`"));
    BEZEL_EXPECT_TRUE(sendableDiagHasRule(r, "rule 3"));
}

// §14.2 / §30.14 / §3.6 rule 4 (method-call site): a record with a class-
// typed field is borrow-bearing per §9.7. The method-call-site diagnostic
// names field `B` + the §9.7 borrow-bearing-record rule.
BEZEL_TEST(sendable_component_diag_2059,
           method_call_record_class_field_names_field_spec_9_7) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenData public Val: int := 0; destructor end destructor; "
        "end OpenData; "
        "record R "
        "  public B: OpenData; "
        "end R; "
        "static class Box "
        "  public static method Hold<T>(x: T): int where T: sendable => 1; "
        "end Box; "
        "class P public static method Main(): int "
        "  return Box.Hold(new R()); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(sendableDiagHasComponent(r, "field `B`"));
    BEZEL_EXPECT_TRUE(sendableDiagHasRule(r, "borrow-bearing"));
}

// ── FALLBACK: function-typed root → no component path (ambiguous verdict) ───

// §3.6 rule 4 (sharpened v0.201, #2059): the walker CONSERVATIVELY returns
// empty on ambiguous verdicts (function-typed root, generic-parameter ref,
// stdlib override) so the diagnostic does not over-promise detail. A
// function-typed argument still produces the §14.2 constraint-failure
// diagnostic, but WITHOUT a component-path fragment. The flat shape —
// "(§14.2)" as the message tail — is preserved.
BEZEL_TEST(sendable_component_diag_2059,
           method_call_function_arg_keeps_flat_message_spec_3_6_rule4) {
    auto r = parseSemaAll(
        "module M; "
        "static class Box "
        "  public static method Hold<T>(x: T): int where T: sendable => 1; "
        "end Box; "
        "static class Twice "
        "  public static method Go(): int => 5; "
        "end Twice; "
        "class C public static method Main(): int "
        "  var f: () -> int := Twice.Go; "
        "  return Box.Hold(f); "
        "end Main; end C;");
    bool sawFlatSendable = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not satisfy the constraint") !=
                std::string::npos &&
            d.message.find("sendable") != std::string::npos) {
            // The flat message ends at "(§14.2)" — no component-path
            // fragment (no ": <component>" tail).
            if (d.message.find("(§14.2):") == std::string::npos)
                sawFlatSendable = true;
        }
    BEZEL_EXPECT_TRUE(sawFlatSendable);
}

// ── INLINE-ARRAY ELEMENT CHAIN ──────────────────────────────────────────────

// §14.2 / §30.14 / §3.6 rule 4: `[N]T` of a class-typed element is borrow-
// bearing (an inline array of class elements aliases externally-owned
// state). The instantiation-site diagnostic names the ELEMENT CLASS `OpenData`
// + the §30.14 value-type rule.
BEZEL_TEST(sendable_component_diag_2059,
           type_inst_inline_array_class_element_names_element_spec_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenData public Val: int := 0; destructor end destructor; "
        "end OpenData; "
        "public class Mailbox<T> where T: sendable "
        "  public constructor() end constructor; "
        "end Mailbox; "
        "class P public static method Main(): int "
        "  using var mb := new Mailbox<[3]OpenData>(); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(sendableDiagHasComponent(r, "element"));
    BEZEL_EXPECT_TRUE(sendableDiagHasComponent(r, "OpenData"));
    BEZEL_EXPECT_TRUE(sendableDiagHasRule(r, "value-type rule"));
}
