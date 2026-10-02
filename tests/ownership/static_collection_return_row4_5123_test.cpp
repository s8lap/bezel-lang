// Spec-derived tests for #5123 — §28.3 announcement table row 4 (spec v0.313,
// ruled at v0.309): a plain (unmarked) STATIC return of a builtin concrete
// collection class names no responsible party and is rejected at the
// declaration. "a trivial-dispose class such as `Array<T>` included (a class
// type, §3.1): it carries the free obligation (K1.6), so its static return
// names no responsible party either". The `owned` spelling announces the
// producer and is accepted; an INSTANCE method keeps its receiver source.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

std::string staticProg(const std::string& retType, const std::string& ctor) {
    return "module M; class C "
           "  public static method Make(): " + retType + " "
           "    return " + ctor + "; "
           "  end Make; "
           "end C;";
}

}  // namespace

BEZEL_TEST(static_collection_return_row4_5123, array_int_plain_static_return_rejected_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("Array<int>", "new Array<int>()"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "static method `Make` returns class/interface type `Array` without `owned`"));
}

BEZEL_TEST(static_collection_return_row4_5123, array_int_owned_static_return_accepted_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("owned Array<int>", "new Array<int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(static_collection_return_row4_5123, dictionary_string_int_plain_static_return_rejected_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("Dictionary<string, int>", "new Dictionary<string, int>()"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "static method `Make` returns class/interface type `Dictionary` without `owned`"));
}

BEZEL_TEST(static_collection_return_row4_5123, dictionary_string_int_owned_static_return_accepted_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("owned Dictionary<string, int>", "new Dictionary<string, int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(static_collection_return_row4_5123, set_int_plain_static_return_rejected_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("Set<int>", "new Set<int>()"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "static method `Make` returns class/interface type `Set` without `owned`"));
}

BEZEL_TEST(static_collection_return_row4_5123, set_int_owned_static_return_accepted_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("owned Set<int>", "new Set<int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(static_collection_return_row4_5123, stack_int_plain_static_return_rejected_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("Stack<int>", "new Stack<int>()"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "static method `Make` returns class/interface type `Stack` without `owned`"));
}

BEZEL_TEST(static_collection_return_row4_5123, stack_int_owned_static_return_accepted_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("owned Stack<int>", "new Stack<int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(static_collection_return_row4_5123, queue_int_plain_static_return_rejected_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("Queue<int>", "new Queue<int>()"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "static method `Make` returns class/interface type `Queue` without `owned`"));
}

BEZEL_TEST(static_collection_return_row4_5123, queue_int_owned_static_return_accepted_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("owned Queue<int>", "new Queue<int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(static_collection_return_row4_5123, linked_list_int_plain_static_return_rejected_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("LinkedList<int>", "new LinkedList<int>()"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "static method `Make` returns class/interface type `LinkedList` without `owned`"));
}

BEZEL_TEST(static_collection_return_row4_5123, linked_list_int_owned_static_return_accepted_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("owned LinkedList<int>", "new LinkedList<int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(static_collection_return_row4_5123, sorted_dictionary_string_int_plain_static_return_rejected_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("SortedDictionary<string, int>", "new SortedDictionary<string, int>()"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "static method `Make` returns class/interface type `SortedDictionary` without `owned`"));
}

BEZEL_TEST(static_collection_return_row4_5123, sorted_dictionary_string_int_owned_static_return_accepted_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("owned SortedDictionary<string, int>", "new SortedDictionary<string, int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(static_collection_return_row4_5123, sorted_set_int_plain_static_return_rejected_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("SortedSet<int>", "new SortedSet<int>()"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "static method `Make` returns class/interface type `SortedSet` without `owned`"));
}

BEZEL_TEST(static_collection_return_row4_5123, sorted_set_int_owned_static_return_accepted_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(staticProg("owned SortedSet<int>", "new SortedSet<int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An INSTANCE method returning a receiver-sourced collection borrow keeps its
// row-4 source (the receiver) and is not rejected by this rule.
BEZEL_TEST(static_collection_return_row4_5123, instance_method_array_return_still_accepted_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private owned items: Array<int> := new Array<int>(); "
        "  public method Items(): Array<int> return items; end Items; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
