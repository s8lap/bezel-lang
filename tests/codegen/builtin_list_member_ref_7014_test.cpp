// fix #7014 (stdlib §4.1, spec §27.6 / §28.9, v0.340): the bare
// method-group REFERENCE to a member of the array-backed BORROWING LIST
// interface `IList<T>` (`il.Contains`, `il.Add`, …).
//
// THE DEFECT. `IList<T>` is the one builtin collection interface whose
// members' CALL form LOWERS: the interface erases nothing that matters,
// because the only builtin implementing it is the array-backed `Array<T>`
// (stdlib §4.1 / §4.4), so `il.Contains(x)` lowers through the identical
// bz_array_* bridge the concrete `Array<T>` uses (the #853 bridge in
// tryLowerCollectionInstanceCall). The member group `il.Contains` has NO
// MethodSymbol (an interface head has no method DECLARATION to bind), so
// codegen's §27.6 method-reference path (`CG60`, LlvmBackend_Exprs.cpp) fell
// through to `ret nullptr`. The local then degraded to a zero value (the
// #2434 warning) and CALLING the method group was a SIGSEGV (measured on the
// base: rc 139, ASan "SEGV on unknown address 0x000000000008").
//
// THE RULE. §27.6 (v0.340): "the member resolved on the value exactly as the
// call `v.Scaled()` is". The call form works here, so the reference form must
// work too — it must be EMITTED, not rejected. (The POLYMORPHIC collection
// interfaces are the opposite case: their call form is fenced by #853, and
// their reference form is rejected to match — the control row below pins
// that this arm did not widen onto them.)
//
// The formed value is a real callable: the group captures the receiver (a
// BORROW, §27.6 "the instance is captured at formation — by reference for
// class- or interface-typed receivers") and its invocation runs the member
// through the SAME lowering the direct call uses. The rows assert the
// OBSERVABLE behaviour (the right value, the right mutation), not merely the
// absence of a crash.
//
// NOT IN SCOPE. The `Dispose` method group over an OWNING COLLECTION
// INTERFACE head (`oc.Dispose`) is a different shape: that receiver's
// disposal path RECLAIMS the collection (`bz_runtime_dispose` frees it —
// IR-verified: the dispose site emits the free and nulls the slot), so a
// group holding a copy of the pointer would free the collection while the
// holder's slot still holds it — a double free. What such a group MEANS is
// the open spec-question #7067.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// Per tests/README.md: a genuine compile-time rejection must be distinguished
// by asserting on the diagnostic TEXT — never by exit code or `!r.ran` alone.
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
// CALL form of the same member already produces on the POLYMORPHIC heads.
const char* kNotYetSupported = "is not yet supported in codegen";

}  // namespace

// The issue's own first repro (stdlib §4.1: `ICollection<T>` declares
// `Contains(item: T): bool`, inherited by `IList<T>`): the group must answer
// exactly as the call does — `True` for the element that is there.
BEZEL_TEST(builtin_list_member_ref_7014,
           spec_27_6_ilist_contains_group_matches_the_call) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "class C public static method Main(): int "
            "  var a: Array<int> := [1,2,3]; "
            "  var il: IList<int> := a; "
            "  var f: (int) -> bool := il.Contains; "
            "  Console.WriteLine(f(2)); "
            "  return 0; "
            "end Main; end C;"),
        "True");
}

// The same formation WITHOUT an annotation: §27.6's formed type is the
// member's own signature, so the binding local takes it and the call
// dispatches through the closure-call path.
BEZEL_TEST(builtin_list_member_ref_7014,
           spec_27_6_ilist_contains_group_without_an_annotation) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "class C public static method Main(): int "
            "  var a: Array<int> := [1,2,3]; "
            "  var il: IList<int> := a; "
            "  var f := il.Contains; "
            "  Console.WriteLine(f(2)); "
            "  return 0; "
            "end Main; end C;"),
        "True");
}

// stdlib §4.1: `IList<T>` declares `Add(item: T)`. The group's invocation
// must append exactly as the call does.
BEZEL_TEST(builtin_list_member_ref_7014,
           spec_27_6_ilist_add_group_appends) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "class C public static method Main(): int "
            "  var a: Array<int> := [1,2,3]; "
            "  var il: IList<int> := a; "
            "  var f: (int) -> () := il.Add; "
            "  f(4); "
            "  Console.WriteLine(il.Count); "
            "  return 0; "
            "end Main; end C;"),
        "4");
}

// stdlib §4.1: `IList<T>` declares `Clear()`.
BEZEL_TEST(builtin_list_member_ref_7014,
           spec_27_6_ilist_clear_group_empties) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "class C public static method Main(): int "
            "  var a: Array<int> := [1,2,3]; "
            "  var il: IList<int> := a; "
            "  var f: () -> () := il.Clear; "
            "  f(); "
            "  Console.WriteLine(il.Count); "
            "  return 0; "
            "end Main; end C;"),
        "0");
}

// stdlib §4.1: `ICollection<T>` declares `Remove(item: T): bool` — "returns
// `true` iff it removed an element, and `false`, leaving the collection
// unchanged, when nothing matched".
BEZEL_TEST(builtin_list_member_ref_7014,
           spec_27_6_ilist_remove_group_removes) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "class C public static method Main(): int "
            "  var a: Array<int> := [1,2,3]; "
            "  var il: IList<int> := a; "
            "  var f: (int) -> bool := il.Remove; "
            "  Console.WriteLine(f(2)); "
            "  Console.WriteLine(il.Count); "
            "  return 0; "
            "end Main; end C;"),
        "True\n2");
}

// stdlib §4.1: `IList<T>` declares `Insert(index: int, item: T)`.
BEZEL_TEST(builtin_list_member_ref_7014,
           spec_27_6_ilist_insert_group_inserts) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "class C public static method Main(): int "
            "  var a: Array<int> := [1,2,3]; "
            "  var il: IList<int> := a; "
            "  var f: (int, int) -> () := il.Insert; "
            "  f(0, 9); "
            "  Console.WriteLine(il.Count); "
            "  Console.WriteLine(il[0]); "
            "  return 0; "
            "end Main; end C;"),
        "4\n9");
}

// The CLASS-element case: §9.2's equality search over a class element reads
// the element's `Equals` — for a class, reference identity (§8.3). The group
// must answer the same as the call.
BEZEL_TEST(builtin_list_member_ref_7014,
           spec_27_6_ilist_contains_group_over_a_class_element) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "public class Item public method Get(): int return 4; end Get; "
            "end Item; "
            "class C public static method Main(): int "
            "  var it := new Item(); "
            "  var a: Array<Item> := [it]; "
            "  var il: IList<Item> := a; "
            "  var f: (Item) -> bool := il.Contains; "
            "  Console.WriteLine(f(it)); "
            "  return 0; "
            "end Main; end C;"),
        "True");
}

// The MEMORY oracle: the closure cell the group is backed by is released at
// scope end (§28.9) and the array is freed exactly once — no leak, no double
// free. `runAotChecked` returns ran=false on any ASan / LSan fire.
BEZEL_TEST(builtin_list_member_ref_7014,
           spec_28_9_ilist_group_is_memory_clean) {
    auto r = runAotChecked(
        "module M; "
        "import System; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var il: IList<int> := a; "
        "  var f: (int) -> bool := il.Contains; "
        "  Console.WriteLine(f(2)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// CONTROL — the CALL form of the same member is untouched: it still answers
// `True`.
BEZEL_TEST(builtin_list_member_ref_7014,
           spec_27_6_ilist_call_form_still_works) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "class C public static method Main(): int "
            "  var a: Array<int> := [1,2,3]; "
            "  var il: IList<int> := a; "
            "  Console.WriteLine(il.Contains(2)); "
            "  return 0; "
            "end Main; end C;"),
        "True");
}

// CONTROL — an ordinary §27.6 method reference on a USER class is untouched:
// the new arm is limited to the builtin list interface's method members.
BEZEL_TEST(builtin_list_member_ref_7014,
           spec_27_6_user_class_method_reference_still_works) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "public class K public method N(): int return 42; end N; end K; "
            "class C public static method Main(): int "
            "  var k: K := new K(); "
            "  var f: () -> int := k.N; "
            "  Console.WriteLine(f()); "
            "  return 0; "
            "end Main; end C;"),
        "42");
}

// CONTROL — the POLYMORPHIC collection interface's member group is still
// REJECTED, exactly as its call form is (the #853 fence): the new arm must
// not have widened onto the heads whose call form does not lower.
BEZEL_TEST(builtin_list_member_ref_7014,
           spec_27_6_polymorphic_interface_group_is_still_rejected) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        "module M; "
        "import System; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var co: ICollection<int> := a; "
        "  var f: (int) -> bool := co.Contains; "
        "  Console.WriteLine(f(2)); "
        "  return 0; "
        "end Main; end C;",
        kNotYetSupported));
}

// fix #7025 (follow-up, §21.1 / §27.6, v0.340): the §21.1 `const` spelling
// of the member-group reference. A §21.1 runtime `const` local's declaration
// is an `ast::LocalConstDecl` (initializer field `value`), NOT an
// `ast::LocalDecl` (field `initializer`), so the binding-local scan missed
// it: the local's declared type was never restored, the call degraded (the
// #2434 warning plus "the argument of this `Console.WriteLine` failed to
// lower to a value, so the whole statement emits nothing — #3079"), and the
// group was a SILENT NO-OP. The row pins the group's real answer.
BEZEL_TEST(builtin_list_member_ref_7014,
           spec_21_1_const_ilist_contains_group_answers_the_call) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "class C public static method Main(): int "
            "  var a: Array<int> := [1,2,3]; "
            "  var il: IList<int> := a; "
            "  const f := il.Contains; "
            "  Console.WriteLine(f(2)); "
            "  return 0; "
            "end Main; end C;"),
        "True");
}
