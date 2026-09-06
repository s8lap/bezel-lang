// Spec-derived regression tests for issue #2087 — the §14.2 "Derived-property
// constraints" admitted-set entry `where T: hashable` (v0.214). The keyword
// names the capability site (hash-container keys/elements); the property keeps
// its §3.6 name, **hashable**. Satisfaction is decided per instantiation
// by the §3.6 hashable computation (isEqualityBearing) — the SAME
// evaluation the §3.6 rule-2 hash-container-key check and the use-site `=`/`<>`
// equality error perform; the constraint "adds no second definition of the
// property" (§14.2). One diagnostic — the constraint failure at the
// instantiation site, naming the constraint + a component path — replaces the
// interior §3.6 rejection the body would otherwise hit.
//
// These tests pin BOTH constraint-check sites:
//   - the method-call site (ExprTypecheck_Calls.cpp's
//     `checkMethodGenericConstraints`), and
//   - the type-instantiation site (Resolver::checkOneConstraint in
//     TypeResolution_Checks.cpp).
//
// The admitted set (§3.6 hashable row, v0.214, corrected in v0.215):
//   ACCEPT: every primitive/string, enum, class, interface, function value,
//           inline array; a positional/combined record; an auto-equality-
//           bearing body record; a record declaring user equality; a value-like
//           auto-hashable choice (§35.7).
//   REJECT: a body record that is neither auto-hashable nor declares
//           user equality; a positional record whose fields fall outside the
//           §9.2 admissible-component rule; an owning choice; a borrow-bearing
//           choice (§35.7 — class-typed payloads are outside the admissible-
//           component rule, exactly as class-typed record fields are).
//
// Authority: spec §14.2 "Derived-property constraints" (v0.214) + Appendix B.2
// (contextual keyword) + §3.6 hashable property (v0.214/v0.215) + §9.2/
// §9.3 (records) + §35.7 (choices). The reject diagnostic *text* (component
// path) is pinned by the lit track (tests/lit/sema/hashable_constraint_2087*).

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::SemaResult;

namespace {

// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

// True iff any `where …: …` constraint-not-satisfied diagnostic was reported.
bool hasConstraintError(const SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not satisfy the constraint") != std::string::npos)
            return true;
    return false;
}

// True iff a constraint-failure diagnostic specifically named `hashable`.
bool hasHashableConstraintError(const SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not satisfy the constraint") != std::string::npos &&
            d.message.find("hashable") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ─── ACCEPT: every admitted hashable category (no false positives) ───

// §14.2 / §3.6: a primitive value type (`int`) is hashable. The
// constraint is satisfied at the method-call site.
BEZEL_TEST(hashable_constraint_2087, method_call_int_accepted_spec_3_6) {
    auto r = parseSemaAll(
        "module M; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<int>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// §14.2 / §3.6: `string` is hashable.
BEZEL_TEST(hashable_constraint_2087, method_call_string_accepted_spec_3_6) {
    auto r = parseSemaAll(
        "module M; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<string>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// §14.2 / §3.6: an enum is hashable (value type with consistent
// Equals/GetHashCode).
BEZEL_TEST(hashable_constraint_2087, method_call_enum_accepted_spec_3_6) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<Color>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// §14.2 / §3.6: a class is hashable (reference identity — consistent
// Equals/GetHashCode). Both a sealed and an open class qualify.
BEZEL_TEST(hashable_constraint_2087, method_call_class_accepted_spec_3_6) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public V: int := 0; destructor end destructor; end Widget; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<Widget>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// §14.2 / §3.6: an interface is hashable (reference identity).
BEZEL_TEST(hashable_constraint_2087, method_call_interface_accepted_spec_3_6) {
    auto r = parseSemaAll(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<IShape>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// §14.2 / §9.1: a positional record synthesizes structural equality over its
// (admissible) fields → hashable.
BEZEL_TEST(hashable_constraint_2087,
           method_call_positional_record_accepted_spec_9_1) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<Point>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// §14.2 / §9.2: an auto-hashable body record (every member an
// auto-property of an admissible component type) is hashable.
BEZEL_TEST(hashable_constraint_2087,
           method_call_auto_eq_body_record_accepted_spec_9_2) {
    auto r = parseSemaAll(
        "module M; "
        "record Tag "
        "  public property Name: string; "
        "  public property Rank: int; "
        "  public constructor(n: string, r: int) Name := n; Rank := r; "
        "  end constructor; "
        "end Tag; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<Tag>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// §14.2 / §25.2: a record OUTSIDE the auto-hashable set (a class-typed
// field makes it non-auto-eq) that DECLARES custom equality (`operator =`/`<>`
// with `Equals(T)`/`GetHashCode`) is hashable.
BEZEL_TEST(hashable_constraint_2087,
           method_call_user_declared_equality_record_accepted_spec_25_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record UserEq "
        "  public Item: R; "
        "  public constructor(i: R) Item := i; end constructor; "
        "  public static method operator =(a: UserEq, b: UserEq): bool => true; "
        "  public static method operator <>(a: UserEq, b: UserEq): bool => false; "
        "  public method Equals(other: UserEq): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end UserEq; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<UserEq>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasHashableConstraintError(r));
}

// §14.2 / §35.7: a value-like auto-hashable choice is hashable.
BEZEL_TEST(hashable_constraint_2087,
           method_call_value_like_choice_accepted_spec_35_7) {
    auto r = parseSemaAll(
        "module M; "
        "choice Opt Some(value: int), None; end Opt; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<Opt>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// §14.2 / §35.7: a payload-free choice is value-like (no payloads) → auto-
// hashable → hashable.
BEZEL_TEST(hashable_constraint_2087,
           method_call_payload_free_choice_accepted_spec_35_7) {
    auto r = parseSemaAll(
        "module M; "
        "choice Suit Hearts, Spades, Clubs, Diamonds; end Suit; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<Suit>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// §14.2 type-instantiation site: `Registry<Point>` against
// `class Registry<K> where K: hashable` is accepted (positional record key).
BEZEL_TEST(hashable_constraint_2087,
           type_inst_positional_record_accepted_spec_9_1) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "public class Registry<K> where K: hashable "
        "  public constructor() end constructor; "
        "end Registry; "
        "class C public static method Main(): int "
        "  using var reg := new Registry<Point>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// ─── REJECT: excluded categories draw the §14.2 constraint diagnostic ────────

// §14.2 / §9.2: a BODY record that is neither auto-hashable (a
// class-typed field makes it borrow-bearing, §9.7) nor declares user equality
// is NOT hashable → rejected at the method-call site naming `hashable`.
BEZEL_TEST(hashable_constraint_2087,
           method_call_non_auto_eq_body_record_rejected_spec_9_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record BodyBad "
        "  public Item: R; "
        "  public constructor(i: R) Item := i; end constructor; "
        "end BodyBad; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<BodyBad>(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasHashableConstraintError(r));
}

// §14.2 / §9.3: a POSITIONAL record whose fields fall outside the §9.2
// admissible-component rule (a class-typed field) does not synthesize equality
// → NOT hashable → rejected at the method-call site.
BEZEL_TEST(hashable_constraint_2087,
           method_call_inadmissible_positional_record_rejected_spec_9_3) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<Holder>(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasHashableConstraintError(r));
}

// §14.2 / §35.7: an owning choice (an `owned` class payload, §35.4.1) is NOT
// value-like auto-hashable → rejected at the method-call site.
BEZEL_TEST(hashable_constraint_2087,
           method_call_owning_choice_rejected_spec_35_7) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "choice FetchOutcome Fetched(owned response: R), NotModified; "
        "end FetchOutcome; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<FetchOutcome>(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasHashableConstraintError(r));
}

// §14.2 / §35.7: a borrow-bearing choice (a plain class payload, §35.4.2) is
// NOT value-like auto-hashable → rejected at the method-call site.
BEZEL_TEST(hashable_constraint_2087,
           method_call_borrow_bearing_choice_rejected_spec_35_7) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "choice Handle Some(value: R), None; end Handle; "
        "static class Box "
        "  public static method Hold<T>(): int where T: hashable => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<Handle>(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasHashableConstraintError(r));
}

// §14.2 type-instantiation site: `Registry<Holder>` against
// `class Registry<K> where K: hashable` is rejected (a positional record whose
// class-typed field leaves it non-hashable, §9.3).
BEZEL_TEST(hashable_constraint_2087,
           type_inst_inadmissible_positional_record_rejected_spec_9_3) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "public class Registry<K> where K: hashable "
        "  public constructor() end constructor; "
        "end Registry; "
        "class C public static method Main(): int "
        "  using var reg := new Registry<Holder>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasHashableConstraintError(r));
}

// ─── COMPOSITION: `where T: hashable, class` ─────────────────────────────────

// §14.2: "Composes with the other forms." A class passes both `hashable` and
// `class`; a primitive fails `class` (the kind constraint runs alongside).
BEZEL_TEST(hashable_constraint_2087,
           method_call_hashable_class_compose_accepted_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public V: int := 0; destructor end destructor; end Widget; "
        "static class Box "
        "  public static method Hold<T>(): int "
        "    where T: hashable, class => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<Widget>(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

BEZEL_TEST(hashable_constraint_2087,
           method_call_int_vs_hashable_class_compose_rejected_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "static class Box "
        "  public static method Hold<T>(): int "
        "    where T: hashable, class => 1; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box.Hold<int>(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// ─── PROPAGATION: a generic `where T: hashable` passes T through ─────────────

// §14.2: "Inside the body, `T` counts as having the property wherever a rule of
// this specification demands it." `Wrapper<T> where T: hashable` instantiates
// `Mailbox<T>` (`Mailbox<M> where M: hashable`) — T carries hashable, so the
// nested constraint is satisfied and the deferred per-instantiation re-check
// passes for a hashable concrete argument (`Wrapper<Point>`). No constraint
// error.
BEZEL_TEST(hashable_constraint_2087,
           propagation_constrained_t_through_nested_generic_accepted_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "public class Mailbox<M2> where M2: hashable "
        "  public constructor() end constructor; "
        "end Mailbox; "
        "public class Wrapper<T> where T: hashable "
        "  public method Make(): int "
        "    using var m := new Mailbox<T>(); "
        "    return 0; "
        "  end Make; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<Point>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// §14.2: the SAME nested instantiation with a NON-hashable concrete argument
// (`Wrapper<Holder>`) is rejected — the outer `where T: hashable` filter fires
// at Wrapper's instantiation site (Holder is a non-auto-eq record).
BEZEL_TEST(hashable_constraint_2087,
           propagation_constrained_t_non_hashable_arg_rejected_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "public class Mailbox<M2> where M2: hashable "
        "  public constructor() end constructor; "
        "end Mailbox; "
        "public class Wrapper<T> where T: hashable "
        "  public method Make(): int "
        "    using var m := new Mailbox<T>(); "
        "    return 0; "
        "  end Make; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper<Holder>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasHashableConstraintError(r));
}

// ─── BARE-T: an UNCONSTRAINED parameter never acquires the property ──────────

// §14.2: "an unconstrained parameter never acquires it." `Registry2<K>` has NO
// `where K: hashable` clause; its interior `Dictionary<K, int>` therefore
// carries its own §3.6 rule-2 per-instantiation obligation, which fires when
// `Registry2<Holder>` binds K to a non-hashable record. (The reject is
// the interior hash-container-key diagnostic, not the §14.2 constraint one —
// there is no constraint here to subsume it.)
BEZEL_TEST(hashable_constraint_2087,
           bare_t_unconstrained_interior_hash_key_rejected_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "public class Registry2<K> "
        "  public method Put(d: Dictionary<K, int>) end Put; "
        "end Registry2; "
        "class C public static method Main(): int "
        "  using var reg := new Registry2<Holder>(); "
        "  return 0; "
        "end Main; end C;");
    bool sawInteriorReject = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("requires a hashable") != std::string::npos &&
            d.message.find("Holder") != std::string::npos)
            sawInteriorReject = true;
    BEZEL_EXPECT_TRUE(sawInteriorReject);
}
