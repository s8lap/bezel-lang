// fix #7014 (stdlib §4.1 / §4.2, spec §27.6, v0.340): the bare
// method-group REFERENCE to a member a builtin COLLECTION INTERFACE head
// declares.
//
// THE DEFECT. A member group formed on a collection-interface receiver —
// `var f := co.Contains;` — has no MethodSymbol (an interface head has no
// method DECLARATION to bind), so codegen's §27.6 method-reference path
// (`CG60`, LlvmBackend_Exprs.cpp) fell through to `ret nullptr`. The local
// then degraded to a zero value (the #2434 warning) and CALLING the method
// group was a SIGSEGV (measured on the base: rc 139, ASan "SEGV on unknown
// address 0x000000000008").
//
// THE RULE. §27.6 (v0.340): "the member resolved on the value exactly as the
// call `v.Scaled()` is". The CALL form of these members is REJECTED by the
// #853 fence in `tryLowerCollectionInstanceCall` — the interface erases the
// concrete backing (array / linked-list / hash-table) and there is no
// collection itable, so no single `bz_*` call is sound. The reference form must
// therefore be rejected identically, and the PROPERTY form on the same heads
// already is (the matching fence in the MemberAccessExpr property arm rejects
// `.Count` through a polymorphic collection interface). Before this fix the
// three forms disagreed: the call and the property were rejected, the
// reference was accepted and crashed.
//
// So these rows assert the REJECTION and the diagnostic TEXT. The pre-fix
// behaviour (accepted, degraded, SIGSEGV) fails them.
//
// NOT IN SCOPE. The array-backed BORROWING LIST interfaces (IReadOnlyList<T> /
// IList<T>) are NOT fenced — their call form lowers through the `bz_array_*`
// bridge, so their method-reference form must be EMITTED, not rejected. That
// needs a closure cell whose scope-end release is armed by the +1-producer
// scan in `LlvmBackend_Setup.cpp`, a file outside the fix's lane; the
// reference form over those heads is deferred (#7014). The control rows below
// pin that the call and property forms over `IList<T>` still compute the
// right values, so the new arm cannot be over-broad.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Per tests/README.md: a genuine compile-time rejection must be distinguished
// by asserting on the diagnostic TEXT — never by exit code or `!r.ran` alone
// (a runtime crash also fails to "run", and the pre-fix behaviour of every
// row below IS a crash).
bool hasErrorContaining(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

// The #853 fence's own wording (LlvmBackend_Calls.cpp) — the diagnostic the
// CALL form of the same member already produces.
const char* kNotYetSupported = "is not yet supported in codegen";

}  // namespace

// stdlib §4.1: `ICollection<T>` declares `Contains(item: T): bool`. The
// reference form must be rejected exactly as the call form is.
BEZEL_TEST(builtin_collection_member_ref_7014,
           icollection_contains_reference_is_rejected_like_the_call) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var co: ICollection<int> := a; "
        "  var f: (int) -> bool := co.Contains; "
        "  Console.WriteLine(f(2)); "
        "  return 0; "
        "end Main; end C;",
        kNotYetSupported));
}

// stdlib §4.2: `IOwningList<T>` declares `Insert(index: int, owned item: T)`.
// This is the issue's own third repro (`ol.Insert`).
BEZEL_TEST(builtin_collection_member_ref_7014,
           iowninglist_insert_reference_is_rejected_like_the_call) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        "module M; "
        "public class Item public method Get(): int return 4; end Get; end Item; "
        "class C public static method Main(): int "
        "  var ol: IOwningList<Item> := new OwningArray<Item>(); "
        "  var f: (int, Item) -> () := ol.Insert; "
        "  f(0, new Item()); "
        "  return 0; "
        "end Main; end C;",
        kNotYetSupported));
}

// stdlib §4.2: `IOwningCollection<T>` declares `Add(owned item: T)`.
BEZEL_TEST(builtin_collection_member_ref_7014,
           iowningcollection_add_reference_is_rejected_like_the_call) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        "module M; "
        "public class Item public method Get(): int return 4; end Get; end Item; "
        "class C public static method Main(): int "
        "  var oc: IOwningCollection<Item> := new OwningArray<Item>(); "
        "  var f: (Item) -> () := oc.Add; "
        "  f(new Item()); "
        "  return 0; "
        "end Main; end C;",
        kNotYetSupported));
}

// stdlib §4.1: `ICollection<T>` declares `Clear()`.
BEZEL_TEST(builtin_collection_member_ref_7014,
           icollection_clear_reference_is_rejected_like_the_call) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var co: ICollection<int> := a; "
        "  var f: () -> () := co.Clear; "
        "  f(); "
        "  return 0; "
        "end Main; end C;",
        kNotYetSupported));
}

// CONTROL — stdlib §4.1: `IReadOnlyCollection<T>` declares NEITHER `Clear` NOR
// `Contains` (it declares only `Count` and `GetEnumerator`), so the SEMANTIC
// rejection is the "no member" one and the #853 fence never applies. The
// diagnostic is unchanged by this fix: the new arm must not turn a no-member
// rejection into the codegen fence's message.
BEZEL_TEST(builtin_collection_member_ref_7014,
           ireadonlycollection_undeclared_member_keeps_the_no_member_diagnostic) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var ro: IReadOnlyCollection<int> := a; "
        "  var f: () -> () := ro.Clear; "
        "  return 0; "
        "end Main; end C;",
        "no member `Clear` on `IReadOnlyCollection`"));
}

// CONTROL — the CALL form over the array-backed `IList<T>` (stdlib §4.1)
// still computes the right value: the new arm must not touch the call path.
BEZEL_TEST(builtin_collection_member_ref_7014,
           ilist_call_form_still_computes_the_value) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var il: IList<int> := a; "
        "  Console.WriteLine(il.Contains(2)); "
        "  return 0; "
        "end Main; end C;"), "True");
}

// CONTROL — the PROPERTY form over the array-backed `IList<T>` still
// computes the right value.
BEZEL_TEST(builtin_collection_member_ref_7014,
           ilist_property_form_still_computes_the_value) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var il: IList<int> := a; "
        "  Console.WriteLine(il.Count); "
        "  return 0; "
        "end Main; end C;"), "3");
}

// CONTROL — an ordinary §27.6 method reference on a USER class is untouched:
// the new arm is limited to the builtin collection interface heads.
BEZEL_TEST(builtin_collection_member_ref_7014,
           user_class_method_reference_still_works) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class K public method N(): int return 42; end N; end K; "
        "class C public static method Main(): int "
        "  var k: K := new K(); "
        "  var f: () -> int := k.N; "
        "  Console.WriteLine(f()); "
        "  return 0; "
        "end Main; end C;"), "42");
}
