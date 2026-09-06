// Spec-derived sema tests for §35.12 (v0.200, #2045): choice sendability.
//
// §35.12 "Sendability": "A choice is sendable iff it is value-like and every
// payload field's type is sendable — the §30.14 value-type rule ('not
// borrow-bearing and not move-only') with the per-field transitivity the
// classification computation already supplies; a sendable choice crosses as
// a copy. Borrow-bearing choices are never sendable (they carry origin-
// domain borrows). Owning choices are not directly sendable — the §30.14
// move-only exclusion, unchanged."
//
// These tests exercise the §30.14 sendable-type computation through the
// `where T: sendable` constraint at a method-call instantiation site
// (the entry point tested by sendable_constraint_2050_test.cpp). The choice
// branch added to Sendability.cpp keys off the §35.4 classifier landed in
// slice 1 of this stage.

#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::TypeSymbol;
using bezel::sema::Symbol;
using bezel::test::parseAndCollect;

namespace {

// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
bezel::test::SemaResult semaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

bool hasSendableConstraintError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not satisfy the constraint") != std::string::npos &&
            d.message.find("sendable") != std::string::npos)
            return true;
    return false;
}

bool hasSendablePayloadDiagnostic(const bezel::test::SemaResult& r,
                                  std::string_view payload,
                                  std::string_view type) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not satisfy the constraint") != std::string::npos &&
            d.message.find("sendable") != std::string::npos &&
            d.message.find(payload) != std::string::npos &&
            d.message.find(type) != std::string::npos &&
            d.message.find("§35.12") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// --- §35.12 ACCEPT: value-like choice IS sendable --------------------------

BEZEL_TEST(choice_sendable_35_12,
           spec_35_12_value_like_choice_satisfies_sendable) {
    // §35.12: a value-like choice is sendable (per §30.14 value-type rule).
    // Substituting a value-like choice for `where T: sendable` is accepted.
    auto r = semaAll(
        "module M; "
        "choice Opt Some(value: int), None; end Opt; "
        "static class Utils public static method Send<T>(): int "
        "  where T: sendable "
        "  return 0; "
        "end Send; end Utils; "
        "class C public static method Main(): int "
        "  return Utils.Send<Opt>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasSendableConstraintError(r));
}

BEZEL_TEST(choice_sendable_35_12,
           spec_35_12_payload_free_choice_satisfies_sendable) {
    // A payload-free choice is value-like by §35.4.3 (no payloads to
    // carry borrows or ownership) and so sendable.
    auto r = semaAll(
        "module M; "
        "choice Color Red, Green, Blue; end Color; "
        "static class Utils public static method Send<T>(): int "
        "  where T: sendable "
        "  return 0; "
        "end Send; end Utils; "
        "class C public static method Main(): int "
        "  return Utils.Send<Color>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasSendableConstraintError(r));
}

BEZEL_TEST(choice_sendable_35_12,
           spec_35_12_string_payload_choice_satisfies_sendable) {
    // §35.12 + §30.14: `string` payloads are sendable. A value-like choice
    // with a string payload is sendable.
    auto r = semaAll(
        "module M; "
        "choice Log Msg(text: string), Empty; end Log; "
        "static class Utils public static method Send<T>(): int "
        "  where T: sendable "
        "  return 0; "
        "end Send; end Utils; "
        "class C public static method Main(): int "
        "  return Utils.Send<Log>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasSendableConstraintError(r));
}

// --- §35.12 REJECT: borrow-bearing choice is NOT sendable ------------------

BEZEL_TEST(choice_sendable_35_12,
           spec_35_12_borrow_bearing_choice_rejects_sendable) {
    // §35.12: "Borrow-bearing choices are never sendable (they carry
    // origin-domain borrows)." A plain (unmarked) class payload makes the
    // choice borrow-bearing per §35.4.2.
    auto r = semaAll(
        "module M; "
        "class C end C; "
        "choice Box Some(value: C), None; end Box; "
        "static class Utils public static method Send<T>(): int "
        "  where T: sendable "
        "  return 0; "
        "end Send; end Utils; "
        "class D public static method Main(): int "
        "  return Utils.Send<Box>(); "
        "end Main; end D;");
    BEZEL_EXPECT_TRUE(hasSendableConstraintError(r));
}

// --- §35.12 REJECT: owning choice is NOT sendable --------------------------

BEZEL_TEST(choice_sendable_35_12,
           spec_35_12_owning_choice_rejects_sendable) {
    // §35.12: "Owning choices are not directly sendable — the §30.14
    // move-only exclusion, unchanged." An `owned` marker on a class
    // payload makes the choice owning per §35.4.1.
    auto r = semaAll(
        "module M; "
        "class R end R; "
        "choice FetchOutcome Fetched(owned response: R), NotModified; "
        "end FetchOutcome; "
        "static class Utils public static method Send<T>(): int "
        "  where T: sendable "
        "  return 0; "
        "end Send; end Utils; "
        "class D public static method Main(): int "
        "  return Utils.Send<FetchOutcome>(); "
        "end Main; end D;");
    BEZEL_EXPECT_TRUE(hasSendableConstraintError(r));
}

// --- §35.12 REJECT: value-like choice with non-sendable payload ------------

BEZEL_TEST(choice_sendable_35_12,
           spec_35_12_value_like_choice_with_unsendable_payload_rejects) {
    // §35.12: "value-like AND every payload field's type is sendable." A
    // value-like choice whose payload is a borrow-bearing record fails the
    // per-field recursion even though the choice itself is value-like
    // (the borrow-bearing record does not make the choice borrow-bearing
    // — wait, it DOES per §35.4.2: "a borrow-bearing record" leg of the
    // borrow-carrying test; so this choice is actually borrow-bearing,
    // and rejects on the verdict, not the per-field recursion). Use a
    // DIFFERENT shape: a value-like choice with an OWNED class payload
    // would be owning (verdict). To isolate the per-field recursion,
    // use a value-like choice with a function-typed payload (function
    // values are never sendable, but a function-typed payload is
    // classification-neutral per §35.4 final paragraph).
    auto r = semaAll(
        "module M; "
        "choice Ev On(f: () -> int), Off; end Ev; "
        "static class Utils public static method Send<T>(): int "
        "  where T: sendable "
        "  return 0; "
        "end Send; end Utils; "
        "class D public static method Main(): int "
        "  return Utils.Send<Ev>(); "
        "end Main; end D;");
    // §35.4 final paragraph: runtime-managed reference types (function
    // types) are classification-neutral → Ev is value-like. §30.14:
    // function-typed payloads are not sendable → per-field recursion
    // fails → the choice rejects despite value-like verdict.
    BEZEL_EXPECT_TRUE(hasSendableConstraintError(r));
}

// --- §35.11 / §35.12: generic choices classify per instantiation (#2211) ----

BEZEL_TEST(choice_sendable_35_12,
           issue_2211_int_substitution_satisfies_sendable_spec_35_11_35_12) {
    // §35.11 / §35.12 (v0.226): classification and sendability are evaluated
    // per instantiation. The bare `T` payload substitutes to sendable `int`, so
    // `Opt<int>` is value-like and every instantiated payload is sendable.
    auto r = semaAll(
        "module M; "
        "choice Opt<T> Some(v: T), None; end Opt; "
        "static class Box public static method S<T>(): int "
        "  where T: sendable => 1; end Box; "
        "class C public static method Main(): int "
        "  return Box.S<Opt<int>>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasSendableConstraintError(r));
}

BEZEL_TEST(choice_sendable_35_12,
           issue_2211_nested_same_choice_satisfies_sendable_spec_35_12) {
    // §35.12 (v0.226): payload sendability is transitive. Re-entering the same
    // generic choice declaration at a finite nested instantiation is not a type
    // cycle: `Opt<Opt<int>>` reaches the sendable `int` leaf and is sendable.
    auto r = semaAll(
        "module M; "
        "choice Opt<T> Some(v: T), None; end Opt; "
        "static class Box public static method S<T>(): int "
        "  where T: sendable => 1; end Box; "
        "class C public static method Main(): int "
        "  return Box.S<Opt<Opt<int>>>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasSendableConstraintError(r));
}

BEZEL_TEST(choice_sendable_35_12,
           issue_2211_recursive_choice_cycle_terminates_and_rejects_35_1_35_12) {
    // §35.1 / §35.12 (v0.226): a choice cannot contain itself by value. The
    // sendability walk must terminate defensively on the recursive payload
    // TypeRef and must not turn that invalid cycle into a sendable type after
    // substitution makes the independent `T` payload (`int`) admissible.
    auto r = semaAll(
        "module M; "
        "choice Node<T> Next(v: Node<T>), Value(v: T); end Node; "
        "static class Box public static method S<T>(): int "
        "  where T: sendable => 1; end Box; "
        "class C public static method Main(): int "
        "  return Box.S<Node<int>>(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasSendableConstraintError(r));
}

BEZEL_TEST(choice_sendable_35_12,
           issue_2211_object_substitution_rejected_with_instantiated_diag_35_12) {
    // §35.12 (v0.226): a value-like verdict is necessary but not sufficient;
    // every instantiated payload must itself be sendable. `Object` is not
    // sendable (§30.14), and the component diagnostic must name `Object`, not
    // the declaration template's bare `T`.
    auto r = semaAll(
        "module M; "
        "choice Opt<T> Some(v: T), None; end Opt; "
        "static class Box public static method S<T>(): int "
        "  where T: sendable => 1; end Box; "
        "class C public static method Main(): int "
        "  return Box.S<Opt<Object>>(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasSendableConstraintError(r));
    BEZEL_EXPECT_TRUE(hasSendablePayloadDiagnostic(
        r, "payload `v`", "type `Object`"));
}

BEZEL_TEST(choice_sendable_35_12,
           issue_2211_class_substitution_remains_rejected_spec_35_12) {
    // §35.11 / §35.12 (v0.226): env-threading may relax only genuinely
    // sendable value substitutions. An unmarked class payload makes `Opt<Res>`
    // borrow-bearing (§35.4.2), hence never sendable.
    auto r = semaAll(
        "module M; "
        "choice Opt<T> Some(v: T), None; end Opt; "
        "class Res public method V(): int => 1; end Res; "
        "static class Box public static method S<T>(): int "
        "  where T: sendable => 1; end Box; "
        "class C public static method Main(): int "
        "  return Box.S<Opt<Res>>(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasSendableConstraintError(r));
}
