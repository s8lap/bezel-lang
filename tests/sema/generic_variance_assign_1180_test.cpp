// Spec-derived tests for §12.9 generic variance assignment enforcement.
// Issue #1180: unsound variant generic assignments (e.g. Array<Dog> to
// IList<Animal>, Array<int> to IEnumerable<long>) were silently accepted;
// the fix enforces §12.9 variance rules in checkAssignable.
//
// §12.9 rules exercised:
//   - Classes, records, and concrete generic types are INVARIANT (L1590).
//   - IList<T> is invariant: IList<Animal> := Array<Dog> is rejected (L1623).
//   - IEnumerable<out T> is covariant:
//       IEnumerable<Animal> := Array<Dog> ACCEPTED  (Dog is ref subtype of Animal)
//       IEnumerable<Object> := Array<Dog> ACCEPTED  (Object is the root)
//       IEnumerable<string> := Array<Dog> REJECTED  (Dog is not a subtype of string)
//   - Value-typed T cannot participate in covariant conversion (L1592):
//       IEnumerable<Object> := Array<int>  REJECTED (int is a value type)
//       IEnumerable<long>   := Array<int>  REJECTED (int is a value type, ≠ long)
//   - IReadOnlyList<out T> is covariant; IReadOnlyList<Animal> := Array<Dog> ACCEPTED.
//   - Exact-arg matches are always accepted regardless of variance annotation.
//
// Test groups:
//   A) Invariant violations          — must be REJECTED.
//   B) Value-type covariance         — must be REJECTED.
//   C) Unrelated reference types     — must be REJECTED.
//   D) Valid assignments             — must be ACCEPTED (anti-over-reject).
//   E) Same user-declared generics   — invariant class check.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Common class hierarchy used across tests.
constexpr std::string_view kHierarchy =
    "module M; class Animal end Animal; class Dog: Animal end Dog; ";

} // namespace

// ---------------------------------------------------------------------------
// Group A: INVARIANT violations — must be REJECTED
// ---------------------------------------------------------------------------

// §12.9 L1623: IList<T> is invariant; IList<Dog> is assignable to IList<Dog>
// only. Assigning Array<Dog> to IList<Animal> is rejected.
BEZEL_TEST(sema_variance_assign, spec_12_9_ilist_animal_from_array_dog_rejected_1180) {
    auto r = parseSemaAll(
        std::string(kHierarchy) +
        "class C public method M(a: Array<Dog>) "
        "  var x: IList<Animal> := a; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ICollection<T> is invariant. ICollection<Animal> := Array<Dog> is rejected.
BEZEL_TEST(sema_variance_assign, spec_12_9_icollection_animal_from_array_dog_rejected_1180) {
    auto r = parseSemaAll(
        std::string(kHierarchy) +
        "class C public method M(a: Array<Dog>) "
        "  var x: ICollection<Animal> := a; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Group B: VALUE-TYPE covariance — must be REJECTED (§12.9 L1592)
// ---------------------------------------------------------------------------

// §12.9 L1592: "For value-typed T, the only legal assignment of IEnumerable<T>
// is to another IEnumerable<T>". Array<int> → IEnumerable<Object> is rejected.
BEZEL_TEST(sema_variance_assign, spec_12_9_ienumerable_object_from_array_int_rejected_1180) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: Array<int>) "
        "  var x: IEnumerable<Object> := a; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Array<int> → IEnumerable<long>: int is a value type, long ≠ int → rejected.
BEZEL_TEST(sema_variance_assign, spec_12_9_ienumerable_long_from_array_int_rejected_1180) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: Array<int>) "
        "  var x: IEnumerable<long> := a; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Group C: UNRELATED reference types — must be REJECTED
// ---------------------------------------------------------------------------

// Array<Dog> → IEnumerable<string>: Dog is not a subtype of string → rejected.
BEZEL_TEST(sema_variance_assign, spec_12_9_ienumerable_string_from_array_dog_rejected_1180) {
    auto r = parseSemaAll(
        std::string(kHierarchy) +
        "class C public method M(a: Array<Dog>) "
        "  var x: IEnumerable<string> := a; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Group D: VALID assignments — must be ACCEPTED (anti-over-reject guard)
// ---------------------------------------------------------------------------

// Exact type-arg match: Array<Dog> → IEnumerable<Dog> — always OK.
BEZEL_TEST(sema_variance_assign, spec_12_9_ienumerable_dog_from_array_dog_accepted_1180) {
    auto r = parseSemaAll(
        std::string(kHierarchy) +
        "class C public method M(a: Array<Dog>) "
        "  var x: IEnumerable<Dog> := a; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Exact type-arg match for IList: Array<Dog> → IList<Dog> — invariant exact match.
BEZEL_TEST(sema_variance_assign, spec_12_9_ilist_dog_from_array_dog_accepted_1180) {
    auto r = parseSemaAll(
        std::string(kHierarchy) +
        "class C public method M(a: Array<Dog>) "
        "  var x: IList<Dog> := a; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Covariant reference types: Array<Dog> → IEnumerable<Animal> (Dog <: Animal) — OK.
BEZEL_TEST(sema_variance_assign, spec_12_9_ienumerable_animal_from_array_dog_accepted_1180) {
    auto r = parseSemaAll(
        std::string(kHierarchy) +
        "class C public method M(a: Array<Dog>) "
        "  var x: IEnumerable<Animal> := a; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Covariant to root: Array<Dog> → IEnumerable<Object> (Dog is a ref type) — OK.
BEZEL_TEST(sema_variance_assign, spec_12_9_ienumerable_object_from_array_dog_accepted_1180) {
    auto r = parseSemaAll(
        std::string(kHierarchy) +
        "class C public method M(a: Array<Dog>) "
        "  var x: IEnumerable<Object> := a; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Exact value-type match: Array<int> → IEnumerable<int> — exact arg, always OK.
BEZEL_TEST(sema_variance_assign, spec_12_9_ienumerable_int_from_array_int_accepted_1180) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: Array<int>) "
        "  var x: IEnumerable<int> := a; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Covariant via IReadOnlyList<out T>: Array<Dog> → IReadOnlyList<Animal> — OK.
BEZEL_TEST(sema_variance_assign, spec_12_9_ireadonlylist_animal_from_array_dog_accepted_1180) {
    auto r = parseSemaAll(
        std::string(kHierarchy) +
        "class C public method M(a: Array<Dog>) "
        "  var x: IReadOnlyList<Animal> := a; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Group E: User-declared INVARIANT generic class — same-declaration check
// ---------------------------------------------------------------------------

// §12.9 L1590: "Classes… are always invariant." Box<Dog> to Box<Animal>
// (Box is a user-declared generic class) must be rejected.
BEZEL_TEST(sema_variance_assign, spec_12_9_user_class_invariant_different_arg_rejected_1180) {
    auto r = parseSemaAll(
        std::string(kHierarchy) +
        "class Box<T> end Box; "
        "class C public method M(b: Box<Dog>) "
        "  var x: Box<Animal> := b; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Box<Dog> to Box<Dog>: same type arg for invariant class — must ACCEPT.
BEZEL_TEST(sema_variance_assign, spec_12_9_user_class_invariant_same_arg_accepted_1180) {
    auto r = parseSemaAll(
        std::string(kHierarchy) +
        "class Box<T> end Box; "
        "class C public method M(b: Box<Dog>) "
        "  var x: Box<Dog> := b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
