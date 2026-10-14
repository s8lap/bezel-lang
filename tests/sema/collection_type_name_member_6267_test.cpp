// Regression tests for issue #6267 (sema, spec §3.4, v0.338).
//
// A bare collection TYPE NAME used as a member-access target WITHOUT a call
// (`Array.Foo;`, `var y := Array.Length;`) named no static member the
// standard library declares, and the builtin-class arm of resolveMemberAccess
// had no fallback reject: the access was accepted silently (rc 0) or degraded
// to a zero-initialized local with only an "internal:" warning. The call form
// `Array.Foo(x)` already reports `'Array' has no static method or property
// 'Foo' (§3.4)` (#6265, typeCall's isArrLike arm); the member-access form now
// reports the same located diagnostic.
//
// Declared statics that must NOT be rejected here (stdlib §4.3-§4.8): the
// `FromCollectionLiteral` literal witness on every sequence collection (§15.4)
// and `FrozenArray.From`. The #3345 static-call sugar `Array.Sort(a)` /
// `Array.Reverse(a)` is a call and is untouched. A user type, a local, or a
// user extension sharing a collection name keeps its own meaning.
//
// per spec §3.4, stdlib §4.3-§4.8 (v0.338).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// A program whose Main body is `body`; the diagnostic must be located and
// carry the exact message `'<cls>' has no static method or property '<m>'`.
bool rejectedNoStatic(const std::string& body, const char* needle) {
    auto r = runAot(
        "module M; class C public static method Main(): int " + body +
        " return 0; end Main; end C;");
    return bezel::test::failedAtCompileTimeWithRealLocation(r) &&
           bezel::test::diagnosticMessageContains(r, needle);
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT: the two issue repros, exact diagnostic, located.
// ---------------------------------------------------------------------------

BEZEL_TEST(collection_type_name_member_6267, issue_repro_expression_statement_rejected) {
    // Issue repro 1: an expression statement naming a nonexistent static.
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "Array.Foo; ",
        "'Array' has no static method or property 'Foo' (\xC2\xA7""3.4)"));
}

BEZEL_TEST(collection_type_name_member_6267, issue_repro_local_initializer_rejected) {
    // Issue repro 2: a local initialized from a nonexistent static property.
    // Before the fix this compiled rc 0 with only an "internal:" degrade
    // warning; the located error is the pin.
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "var y := Array.Length; ",
        "'Array' has no static method or property 'Length' (\xC2\xA7""3.4)"));
}

// ---------------------------------------------------------------------------
// REJECT: each collection name, and each expression position.
// ---------------------------------------------------------------------------

BEZEL_TEST(collection_type_name_member_6267, every_collection_name_statement_rejected) {
    const char* names[] = {
        "Array",      "OwningArray",      "Stack",       "OwningStack",
        "Queue",      "OwningQueue",      "Set",         "OwningSet",
        "LinkedList", "OwningLinkedList", "Dictionary",  "OwningDictionary",
        "FrozenArray",
    };
    for (const char* n : names) {
        const std::string needle =
            std::string("'") + n + "' has no static method or property 'Foo' (\xC2\xA7""3.4)";
        BEZEL_EXPECT_TRUE(rejectedNoStatic(std::string(n) + ".Foo; ", needle.c_str()));
    }
}

BEZEL_TEST(collection_type_name_member_6267, collection_name_local_initializer_rejected) {
    // Other collection names in the local-initializer position.
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "var y := Set.Length; ",
        "'Set' has no static method or property 'Length' (\xC2\xA7""3.4)"));
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "var y := Dictionary.Length; ",
        "'Dictionary' has no static method or property 'Length' (\xC2\xA7""3.4)"));
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "var y := Stack.Length; ",
        "'Stack' has no static method or property 'Length' (\xC2\xA7""3.4)"));
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "var y := OwningDictionary.Length; ",
        "'OwningDictionary' has no static method or property 'Length' (\xC2\xA7""3.4)"));
}

BEZEL_TEST(collection_type_name_member_6267, bare_instance_member_on_type_name_rejected) {
    // A non-static member (instance method / property) named on the type name
    // is not a static member: rejected the same way.
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "Array.Sort; ",
        "'Array' has no static method or property 'Sort' (\xC2\xA7""3.4)"));
}

BEZEL_TEST(collection_type_name_member_6267, collection_name_argument_position_rejected) {
    auto r = runAot(
        "module M; class C "
        "  public static method Sink(v: int): int return v; end Sink; "
        "  public static method Main(): int "
        "    return Sink(Set.Length); "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "'Set' has no static method or property 'Length' (\xC2\xA7""3.4)"));
}

BEZEL_TEST(collection_type_name_member_6267, collection_name_condition_position_rejected) {
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "if Array.Length = 0 then return 1; end; ",
        "'Array' has no static method or property 'Length' (\xC2\xA7""3.4)"));
}

BEZEL_TEST(collection_type_name_member_6267, collection_name_operand_position_rejected) {
    BEZEL_EXPECT_TRUE(rejectedNoStatic(
        "var t: int := Array.Length + 1; ",
        "'Array' has no static method or property 'Length' (\xC2\xA7""3.4)"));
}

BEZEL_TEST(collection_type_name_member_6267, collection_name_field_initializer_rejected) {
    // Instance field initializer position: the member-access target is the
    // bare collection name, outside any method body.
    auto r = runAot(
        "module M; class C "
        "  public n: int := Array.Length; "
        "  public static method Main(): int "
        "    var c: C := new C(); return c.n; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "'Array' has no static method or property 'Length' (\xC2\xA7""3.4)"));
}

// ---------------------------------------------------------------------------
// ACCEPT controls: real static sugar and declared statics still work.
// ---------------------------------------------------------------------------

BEZEL_TEST(collection_type_name_member_6267, static_sort_and_reverse_sugar_accepted) {
    // #3345 static-call sugar is a CALL, so the member-access arm never sees
    // it. Value check: [3,1,2] -> Sort -> [1,2,3] -> Reverse -> [3,2,1].
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1, 2]; "
        "  Array.Sort(a); "
        "  Array.Reverse(a); "
        "  return a[0]; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

BEZEL_TEST(collection_type_name_member_6267, generic_instantiation_static_sugar_accepted) {
    // `Array<int>.Sort(a)` (generic-instantiation spelling of the same sugar):
    // [3,1,2] -> Sort -> [1,2,3]; return a[0] = 1.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1, 2]; "
        "  Array<int>.Sort(a); "
        "  return a[0]; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(collection_type_name_member_6267, local_named_like_collection_accepted) {
    // A local named `Array` shadows the type name: `Array` is a value, not a
    // type-name target, so no static lookup happens.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var Array: int := 7; "
        "  return Array; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(collection_type_name_member_6267, user_class_named_list_with_statics_accepted) {
    // A user class named `List` (no stdlib List exists) with its own statics.
    auto r = runAot(
        "module M; "
        "class List "
        "  public static method Foo(): int return 2; end Foo; "
        "  public static method Len(): int return 4; end Len; "
        "end List; "
        "class C public static method Main(): int "
        "  return List.Foo() + List.Len(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

BEZEL_TEST(collection_type_name_member_6267, user_class_named_stack_with_static_accepted) {
    // A user class shadowing a collection name keeps its own static member.
    auto r = runAot(
        "module M; "
        "class Stack public static method Foo(): int return 3; end Foo; end Stack; "
        "class C public static method Main(): int "
        "  return Stack.Foo(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

BEZEL_TEST(collection_type_name_member_6267, collection_instance_extension_accepted) {
    // A user extension on Array<T>: an instance call on a value, unaffected.
    auto r = runAot(
        "module M; "
        "public extension Array<T> where T: IComparable<T> "
        "  public method Twice(): int return 2; end Twice; "
        "end; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [3, 1]; "
        "  return a.Twice(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

BEZEL_TEST(collection_type_name_member_6267, system_qualified_object_static_accepted) {
    // The `System.` pseudo-module chain head (§8.1) reaches the Object static
    // without an import; the member-access arm does not see it.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1]; "
        "  if System.Object.ReferenceEquals(a, a) then return 4; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

BEZEL_TEST(collection_type_name_member_6267, dictionary_constructed_with_new_accepted) {
    // Construction through `new` (not a static on the type name) is unaffected.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var d: Dictionary<string, int> := new Dictionary<string, int>(); "
        "  d[\"a\"] := 1; "
        "  return d.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
