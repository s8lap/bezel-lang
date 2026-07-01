// Spec-derived tests for Sema slice S23 — record restrictions (§9.7).
// Records may not have a destructor, an `owned` field, an owning
// inline-array field, or implement `IDisposable`.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

BEZEL_TEST(sema_record, owned_field_diagnoses) {
    auto r = parseSemaAll(
        "module M; class T end T; "
        "record R public owned _t: T; end R;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_record, destructor_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "record R public _x: int := 0; destructor end destructor; end R;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_record, implements_idisposable_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "record R : IDisposable public _x: int := 0; end R;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_record, plain_record_ok) {
    auto r = parseSemaAll(
        "module M; "
        "record R public _x: int := 0; end R;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_record, class_owned_field_ok) {
    // The restriction is record-specific; a class may own a field.
    auto r = parseSemaAll(
        "module M; class T end T; "
        "class C private owned _t: T := new T(); end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #1272 (§9): a record is a value type stored inline, so a record that
// transitively contains itself by value has infinite size and cannot be laid
// out. Left unchecked this crashed codegen (unbounded record-struct layout
// recursion → LLVM setBody). It must be a clean sema reject. A recursive
// structure must use a class (a heap reference), which breaks the cycle.

// Direct self-reference through a NULLABLE field (`Node?` is still stored inline).
BEZEL_TEST(sema_record, recursive_value_record_nullable_self_rejected_1272) {
    auto r = parseSemaAll(
        "module M; record Node(Val: int, Next: Node?); ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Direct self-reference through a NON-nullable field.
BEZEL_TEST(sema_record, recursive_value_record_nonnullable_self_rejected_1272) {
    auto r = parseSemaAll(
        "module M; record Node(Val: int, Next: Node); ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Mutual recursion A→B→A.
BEZEL_TEST(sema_record, recursive_value_record_mutual_rejected_1272) {
    auto r = parseSemaAll(
        "module M; record A(b: B?, x: int); record B(a: A?, y: int); ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// An inline array of the record itself is still an inline value cycle.
BEZEL_TEST(sema_record, recursive_value_record_inline_array_rejected_1272) {
    auto r = parseSemaAll(
        "module M; record Tree(kids: [3]Tree, v: int); ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// CONTROL: a record referencing a CLASS (heap reference) is NOT a value cycle —
// the class field breaks it. Must compile.
BEZEL_TEST(sema_record, record_with_class_field_not_rejected_1272) {
    auto r = parseSemaAll(
        "module M; class Node public V: int; "
        "  public constructor(v: int) V := v; end constructor; end Node; "
        "record Ref(n: Node, tag: int); ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a record containing a DIFFERENT (non-cyclic) record is fine.
BEZEL_TEST(sema_record, record_in_record_no_cycle_not_rejected_1272) {
    auto r = parseSemaAll(
        "module M; record Inner(x: int); record Outer(i: Inner, y: int); ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
