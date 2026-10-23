// Sema — #7027 / stdlib §4.1, §4.2 + language spec §27.6 (v0.340).
//
// THE DEFECT. An undeclared CALL on one of the four POLYMORPHIC collection
// interface heads was rejected with the #853 codegen backstop's message,
// which says the member EXISTS and is missing only from codegen:
//
//     var a: Array<int> := [1,2,3];
//     var co: ICollection<int> := a;
//     co.Insert(0, 1);
//
// Diagnostic: "`.Insert` through a polymorphic collection interface
// (IReadOnlyCollection<T> / ICollection<T> / IOwningCollection<T> /
// IOwningList<T>) is not yet supported in codegen (stdlib §4.1–§4.3, #853)".
//
// But stdlib §4.1 declares `ICollection<T>` with `IsReadOnly`, `Add`, `Clear`,
// `Contains` and `Remove` — `Insert` is declared on `IList<T>` only — so
// `co.Insert` names NO member of that head, and "missing only from codegen" is
// false. The issue is a WORDING defect, not a verdict defect: the program was
// (and still is) rejected.
//
// THE RULE. Language spec §27.6 resolves a member "on the value exactly as the
// call `v.Scaled()` is", so the two spellings of one undeclared name must
// agree, and the rejection must name the RIGHT reason. Fix #7015 moved the
// call form to the type level on the four heads the #853 backstop does not
// reach; this fix does the same for the four it does reach, so the "no
// member" wording answers the call on all eight heads.
//
// A DECLARED member on a polymorphic head KEEPS #853's message — there the
// member exists and only codegen's lowering is missing (stdlib §4.1–§4.3,
// #853 stays open). CONTROL 1 pins that.
//
// ORACLE. `runAot`. The reject rows assert the diagnostic by needle and that
// it was a compile-time reject with a real location, so a crash or an
// unrelated compile error cannot pass. The control rows assert the diagnostic
// that must still be there, or EXACT stdout.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Assert the program is REJECTED at compile time with `needle` in the message.
// `!r.ran` alone would pass on any compile error at all.
void expectRejected(const std::string& prog, const std::string& needle) {
    JitResult r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle.c_str()));
}

// Assert the program COMPILES, runs to completion, and prints exactly `out`.
void expectStdout(const std::string& prog, const std::string& out) {
    JitResult r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// A `Main` over a borrowing `Array<int>` [1, 2, 3], so each row differs only
// in the one call under test.
std::string mainWith(const std::string& locals, const std::string& stmts) {
    return "module M7027; class C public static method Main(): int "
           "var a: Array<int> := [1, 2, 3]; " +
           locals + stmts + "return 0; end Main; end C;";
}

// The same, over an owning `OwningArray<Item>`, for the §4.2 family.
std::string owningMainWith(const std::string& locals, const std::string& stmts) {
    return "module M7027; public class Item public method Get(): int "
           "return 4; end Get; end Item; "
           "class C public static method Main(): int " +
           locals + stmts + "return 0; end Main; end C;";
}

}  // namespace

// ===========================================================================
// THE REJECTIONS — a call to a name the interface does not declare.
// ===========================================================================

// ROW 1 — the issue's own repro: `Insert` is declared on `IList<T>`
// (stdlib §4.1) and on no other head, so `co.Insert` names no member of
// `ICollection<T>`. Before this fix the diagnostic was #853's "not yet
// supported in codegen", which claims the member exists.
BEZEL_TEST(builtin_interface_undeclared_member_call_7027,
           icollection_call_insert_is_rejected_spec_4_1) {
    expectRejected(
        mainWith("var co: ICollection<int> := a; ", "co.Insert(0, 1); "),
        "no member `Insert` on `ICollection`");
}

// ROW 2 — a name no head declares at all, on the same head.
BEZEL_TEST(builtin_interface_undeclared_member_call_7027,
           icollection_call_nope_is_rejected_spec_4_1) {
    expectRejected(
        mainWith("var co: ICollection<int> := a; ", "co.Nope(1); "),
        "no member `Nope` on `ICollection`");
}

// ROW 3 — the read-only collection head (stdlib §4.1) declares `Count` and
// `GetEnumerator` only.
BEZEL_TEST(builtin_interface_undeclared_member_call_7027,
           ireadonlycollection_call_nope_is_rejected_spec_4_1) {
    expectRejected(
        mainWith("var rc: IReadOnlyCollection<int> := a; ", "rc.Nope(1); "),
        "no member `Nope` on `IReadOnlyCollection`");
}

// ROW 4 — the owning collection head (stdlib §4.2).
BEZEL_TEST(builtin_interface_undeclared_member_call_7027,
           iowningcollection_call_nope_is_rejected_spec_4_2) {
    expectRejected(
        owningMainWith("var oc: IOwningCollection<Item> := new OwningArray<Item>(); ",
                        "oc.Nope(1); "),
        "no member `Nope` on `IOwningCollection`");
}

// ROW 5 — the owning list head (stdlib §4.2).
BEZEL_TEST(builtin_interface_undeclared_member_call_7027,
           iowninglist_call_nope_is_rejected_spec_4_2) {
    expectRejected(
        owningMainWith("var ol: IOwningList<Item> := new OwningArray<Item>(); ",
                        "ol.Nope(1); "),
        "no member `Nope` on `IOwningList`");
}

// ROW 6 — the null-conditional spelling reaches a separate sema block, and
// the two spellings must agree there too (language spec §27.6).
BEZEL_TEST(builtin_interface_undeclared_member_call_7027,
           icollection_null_conditional_call_nope_is_rejected_spec_27_6) {
    expectRejected(
        mainWith("var co: ICollection<int> := a; ", "co?.Nope(1); "),
        "no member `Nope` on `ICollection`");
}

// ===========================================================================
// THE CONTROLS — what must NOT change.
// ===========================================================================

// CONTROL 1 — a DECLARED member on a polymorphic head keeps #853's message:
// the member EXISTS and only codegen's lowering is missing (stdlib §4.1–§4.3,
// #853 stays open). `Add` is declared on `ICollection<T>` (stdlib §4.1), so
// this rejection must keep saying "not yet supported in codegen" and must NOT
// become a "no member" diagnostic.
BEZEL_TEST(builtin_interface_undeclared_member_call_7027,
           icollection_add_call_still_rejected_with_853_wording_spec_4_1) {
    expectRejected(
        mainWith("var co: ICollection<int> := a; ", "co.Add(1); "),
        "is not yet supported in codegen");
}

// CONTROL 2 — the same for the owning collection head, whose `Add` takes an
// owned element (stdlib §4.2).
BEZEL_TEST(builtin_interface_undeclared_member_call_7027,
           iowningcollection_add_call_still_rejected_with_853_wording_spec_4_2) {
    expectRejected(
        owningMainWith("var oc: IOwningCollection<Item> := new OwningArray<Item>(); ",
                        "oc.Add(move (new Item())); "),
        "is not yet supported in codegen");
}

// CONTROL 3 — a stdlib §5 operator name is not a member of the head, and the
// call is legitimate (§5): the reject must not fire on it. (`Sum` on a
// polymorphic head is refused by the #2457 backstop, not by this rule — the
// row pins that the "no member" wording does NOT appear.)
BEZEL_TEST(builtin_interface_undeclared_member_call_7027,
           icollection_linq_sum_call_is_not_a_no_member_reject_spec_5) {
    JitResult r = runAot(mainWith("var co: ICollection<int> := a; ",
                                   "Console.WriteLine(co.Sum()); "));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "no member `Sum`"));
}

// CONTROL 4 — the `?.` spelling of a stdlib §5 operator name, likewise.
BEZEL_TEST(builtin_interface_undeclared_member_call_7027,
           icollection_null_conditional_linq_sum_call_is_not_a_no_member_reject_spec_5) {
    JitResult r = runAot(mainWith("var co: ICollection<int> := a; ",
                                   "co?.Sum(); "));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "no member `Sum`"));
}

// CONTROL 5 — a user extension that applies to the receiver (language spec
// §26.5) is what the call resolves to, so it is not rejected.
BEZEL_TEST(builtin_interface_undeclared_member_call_7027,
           user_extension_call_on_icollection_still_works_spec_26_5) {
    expectStdout(
        "module M7027; "
        "extension ICollection<int> public method Tag(): int => 42; end; "
        "class C public static method Main(): int "
        "var a: Array<int> := [1, 2, 3]; "
        "var co: ICollection<int> := a; "
        "Console.WriteLine(co.Tag()); "
        "return 0; end Main; end C;",
        "42\n");
}
