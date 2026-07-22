// Sema — §35.7 / §3.6 / §14.2 (v0.210/v0.214, #2083 × #2087 merged rule):
// `isEqualityBearing` for a CHOICE applies the RECURSIVE §35.7 rule — a choice
// is equality-bearing iff it is value-like AND every payload field of every
// case is a §9.2 admissible component ("this same list applied to every case's
// payload fields"). The pre-merge #2087 commit used the §35.4 classification
// ALONE, which over-admitted a value-like choice with an inadmissible payload
// (function-typed / Object-typed payloads are ValueLike by classification but
// outside the §9.2 admissible list) — the KEY-POSITION half of #2110. Under
// the merged rule that half is fixed: such a choice is rejected as a hash-
// container key (§3.6 rule 2) and fails `where T: hashable` (§14.2), with the
// component path naming the offending case/payload.
//
// (The USE-SITE half of #2110 — the §35.7 `=`/`<>` use-site error still being
// classification-only — stays open; these tests do not pin it.)

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::SemaResult;

namespace {

SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasErrDiagWith(const SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ─── #2110 KEY-POSITION HALF (fixed by the merged recursive rule) ────────────

// §3.6 rule 2 / §35.7: a VALUE-LIKE choice with a FUNCTION-TYPED payload is
// NOT equality-bearing under the recursive rule (a function value is outside
// the §9.2 admissible-component list) → rejected as a Dictionary key. The
// pre-merge classification-only rule admitted it (the #2110 repro shape).
BEZEL_TEST(choice_recursive_equality_rule_2083_2087,
           spec_3_6_v210_function_payload_choice_dictionary_key_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "choice Holder Fn(callback: (int) -> int), None; end Holder; "
        "class C public static method Main(): int "
        "  var d := new Dictionary<Holder, int>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "requires an equality-bearing"));
    // The reason clause is the value-like-specific one (not the wrong
    // "borrow-bearing" wording the pre-amendment reasonClause produced).
    BEZEL_EXPECT_TRUE(hasErrDiagWith(
        r, "value-like but has a case payload outside"));
}

// §3.6 rule 2 / §35.7: same shape with an OBJECT-typed payload (`Object` is a
// reference type, outside the §9.2 admissible list, yet ValueLike by the
// §35.4 classification legs).
BEZEL_TEST(choice_recursive_equality_rule_2083_2087,
           spec_3_6_v210_object_payload_choice_dictionary_key_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "choice Boxy Boxed(item: Object), None; end Boxy; "
        "class C public static method Main(): int "
        "  var d := new Dictionary<Boxy, int>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "requires an equality-bearing"));
}

// §14.2 / §35.7: the SAME choice fails `where T: hashable` — the constraint
// routes through the one shared isEqualityBearing computation ("no second
// definition"), so the recursive rule decides both sites identically. The
// diagnostic names the offending case/payload component (finding 3's
// hashableFailureComponentPath extension).
BEZEL_TEST(choice_recursive_equality_rule_2083_2087,
           spec_14_2_v214_function_payload_choice_fails_hashable_with_component_path) {
    auto r = parseSemaAll(
        "module M; "
        "choice Holder Fn(callback: (int) -> int), None; end Holder; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<Holder>(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "does not satisfy the constraint"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "hashable"));
    // Component path: names the case and the payload field.
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r, "case `Fn` payload `callback`"));
}

// ─── NO OVER-REJECT: admissible-payload value-like choices stay accepted ─────

// §3.6 / §35.7: a value-like choice whose payloads are all admissible (string,
// int) IS equality-bearing under the recursive rule — Dictionary key accepted
// (the recursive tightening must not over-reject the admitted set).
BEZEL_TEST(choice_recursive_equality_rule_2083_2087,
           spec_3_6_v210_string_payload_choice_dictionary_key_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "choice Msg Note(text: string), Count(n: int), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var d := new Dictionary<Msg, int>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r, "requires an equality-bearing"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §14.2 / §35.7: the same admitted choice satisfies `where T: hashable`.
BEZEL_TEST(choice_recursive_equality_rule_2083_2087,
           spec_14_2_v214_string_payload_choice_satisfies_hashable) {
    auto r = parseSemaAll(
        "module M; "
        "choice Msg Note(text: string), Count(n: int), Empty; end Msg; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<Msg>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r, "does not satisfy the constraint"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
