// Sema — #6997 / stdlib §4.1, §4.2, §4.12, §4.13 + language spec §27.6, §8.1 (v0.340).
//
// THE DEFECT. A BARE (non-call) reference to a name a builtin collection
// interface does not declare was ACCEPTED:
//
//     var il: IList<int> := a;
//     var g: (int) -> int := il.Nope;     // compiled; `g` zero-initialized (#2434)
//     Console.WriteLine(g(5));            // SEGV
//
// The reference was left untyped, so codegen degraded `g` to a warning and the
// call crashed. The CALL form `il.Nope(5)` was already rejected by the #2457
// backstop, so the two spellings of one undeclared name disagreed.
//
// THE RULE. A method reference is a use of the member, resolved on the receiver
// exactly as the call is: "the member resolved on the value exactly as the call
// `v.Scaled()` is" (language spec §27.6). A name the call form rejects as not a
// member is not a member for the bare form either, so the bare form is rejected
// with the same "no member" wording the other interface rejects use.
//
// THE SURFACE. The members an interface declares are the ones the stdlib
// documents for it, the union over its inherited ancestors: `IReadOnlyList<T>`
// has `Count` and `GetEnumerator` (§4.1); `IList<T>` adds `Insert` and the
// indexer; the owning pair (§4.2) adds `Dispose` to the collection members;
// the enumeration pair (§4.12, §4.13) has `GetEnumerator` /
// `GetOwningEnumerator` / `Dispose`. Three things are NOT rejected here, and
// each has a control row:
//   * the universal `Object` members (language spec §8.1: `GetType`, `Equals`,
//     `GetHashCode`, `ToString`, static `ReferenceEquals`);
//   * the stdlib §5 LINQ operator names (`il.Count()`), which resolve through
//     the LINQ typing and not through the member surface;
//   * a user extension that applies to the receiver (§26.5), which the bare
//     reference resolves to exactly as the call does (`il.Tag`).
//
// ORACLE. `runAot`. The reject rows assert the diagnostic by needle and that it
// was a compile-time reject with a real location, so a crash or an unrelated
// compile error cannot pass. The control rows assert EXACT stdout, so a fix
// that over-rejects, or that compiles a bare reference to a value-less local,
// fails here.

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

// A `Main` over a borrowing `Array<int>` [1, 2, 3], so each row differs only in
// the one reference under test.
std::string mainWith(const std::string& locals, const std::string& stmts) {
    return "module M6997; class C public static method Main(): int "
           "var a: Array<int> := [1, 2, 3]; " +
           locals + stmts + "return 0; end Main; end C;";
}

// The same, over an owning `OwningArray<Item>`, for the §4.2 / §4.13 family.
std::string owningMainWith(const std::string& locals, const std::string& stmts) {
    return "module M6997; public class Item public method Get(): int "
           "return 4; end Get; end Item; "
           "class C public static method Main(): int " +
           locals + stmts + "return 0; end Main; end C;";
}

}  // namespace

// ===========================================================================
// THE REJECTIONS — a bare reference to a name the interface does not declare.
// ===========================================================================

// ROW 1 — the issue's own repro: `il.Nope` as a function-typed local, then called.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ilist_bare_nope_method_group_is_rejected_spec_27_6) {
    expectRejected(
        mainWith("var il: IList<int> := a; ",
                 "var g: (int) -> int := il.Nope; Console.WriteLine(g(5)); "),
        "no member `Nope` on `IList`");
}

// ROW 2 — a read-only list has no `Add`; the bare group over it is rejected.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ireadonlylist_bare_add_is_rejected_spec_4_1) {
    expectRejected(
        mainWith("var ro: IReadOnlyList<int> := a; ",
                 "var f: (int) -> () := ro.Add; "),
        "no member `Add` on `IReadOnlyList`");
}

// ROW 3 — `IReadOnlyCollection<T>` declares `Count` only, not `Nope`.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ireadonlycollection_bare_nope_is_rejected_spec_4_1) {
    expectRejected(
        mainWith("var rc: IReadOnlyCollection<int> := a; ",
                 "var f := rc.Nope; "),
        "no member `Nope` on `IReadOnlyCollection`");
}

// ROW 4 — `IList<T>` is not disposable: `Dispose` is not on its surface.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ilist_bare_dispose_is_rejected_spec_4_1) {
    expectRejected(
        mainWith("var il: IList<int> := a; ", "var f := il.Dispose; "),
        "no member `Dispose` on `IList`");
}

// ROW 5 — `ICollection<T>` has no `Push`; the bare group over it is rejected.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           icollection_bare_push_is_rejected_spec_4_1) {
    expectRejected(
        mainWith("var c: ICollection<int> := a; ",
                 "var f: (int) -> () := c.Push; "),
        "no member `Push` on `ICollection`");
}

// ROW 6 — `IEnumerable<T>` declares `GetEnumerator` only.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ienumerable_bare_push_is_rejected_spec_4_12) {
    expectRejected(
        mainWith("var e: IEnumerable<int> := a; ",
                 "var f: (int) -> () := e.Push; "),
        "no member `Push` on `IEnumerable`");
}

// ROW 7 — the owning list (§4.2): an undeclared name is rejected bare.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           iowninglist_bare_nope_is_rejected_spec_4_2) {
    expectRejected(
        owningMainWith("var ol: IOwningList<Item> := new OwningArray<Item>(); ",
                       "var f := ol.Nope; "),
        "no member `Nope` on `IOwningList`");
}

// ROW 8 — the owning collection (§4.2).
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           iowningcollection_bare_push_is_rejected_spec_4_2) {
    expectRejected(
        owningMainWith("var oc: IOwningCollection<Item> := new OwningArray<Item>(); ",
                       "var f := oc.Push; "),
        "no member `Push` on `IOwningCollection`");
}

// ROW 9 — the owning enumerable (§4.13): `GetOwningEnumerator` and `Dispose`
// are its surface, nothing else.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           iowningenumerable_bare_nope_is_rejected_spec_4_13) {
    expectRejected(
        owningMainWith("var oe: IOwningEnumerable<Item> := new OwningArray<Item>(); ",
                       "var f := oe.Nope; "),
        "no member `Nope` on `IOwningEnumerable`");
}

// ROW 10 — the null-conditional spelling reaches a separate sema block and is
// rejected by the same rule.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ilist_null_conditional_bare_nope_is_rejected_spec_27_6) {
    expectRejected(
        mainWith("var il: IList<int> := a; ", "var f := il?.Nope; "),
        "no member `Nope` on `IList`");
}

// ROW 11 — the interpolation hole is a member reference too.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ilist_interpolated_nope_is_rejected_spec_27_6) {
    expectRejected(
        mainWith("var il: IList<int> := a; ",
                 "Console.WriteLine($\"{il.Nope}\"); "),
        "no member `Nope` on `IList`");
}

// ROW 12 — the CALL form is untouched: it stays rejected by the #2457 codegen
// backstop, with its own wording, and this fix does not change that message.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ilist_call_nope_stays_rejected_by_2457_backstop) {
    expectRejected(
        mainWith("var il: IList<int> := a; ",
                 "Console.WriteLine(il.Nope(5)); "),
        "`Nope` on built-in type `IList` produced no value");
}

// ===========================================================================
// THE CONTROLS — the surfaces that must keep compiling, with exact stdout.
// ===========================================================================

// CONTROL 1 — a documented property read on the same interface.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ilist_count_read_still_prints_spec_4_1) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "Console.WriteLine(il.Count); "),
                 "3\n");
}

// CONTROL 2 — the §5 LINQ operator spelled as a call. `Count` is a §5 name, so
// it must not be taken for an undeclared member.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ilist_linq_count_call_still_works_spec_5) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "Console.WriteLine(il.Count()); "),
                 "3\n");
}

// CONTROL 3 — a documented call form on the same interface.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ilist_contains_call_still_works_spec_4_1) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "Console.WriteLine(il.Contains(2)); "),
                 "True\n");
}

// CONTROL 4 — the documented property read inside an interpolation hole.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ilist_interpolated_count_still_prints_spec_4_1) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "Console.WriteLine($\"{il.Count}\"); "),
                 "3\n");
}

// CONTROL 5 — a documented bare method group still compiles (not called here,
// so this row pins the reference's acceptance and not the group's runtime).
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           ienumerable_bare_get_enumerator_group_still_compiles_spec_4_12) {
    expectStdout(mainWith("var e: IEnumerable<int> := a; ",
                          "var f := e.GetEnumerator; Console.WriteLine(1); "),
                 "1\n");
}

// CONTROL 6 — a same-module user extension applies to the receiver (§26.5), so
// the call `il.Tag()` resolves to it and the bare reference is not rejected.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           user_extension_call_on_ilist_still_works_spec_26_5) {
    expectStdout(
        "module M6997; "
        "extension IList<int> public method Tag(): int => 42; end; "
        "class C public static method Main(): int "
        "var a: Array<int> := [1, 2, 3]; "
        "var il: IList<int> := a; "
        "Console.WriteLine(il.Tag()); "
        "return 0; end Main; end C;",
        "42\n");
}

// CONTROL 7 — the same applies to a BARE extension method group (§27.6, the
// receiver-qualified form): `Tag2` is undocumented anywhere, so only the
// extension lookup keeps this reference from being rejected.
BEZEL_TEST(builtin_interface_nonexistent_member_ref_6997,
           user_extension_bare_group_on_ienumerable_still_works_spec_27_6) {
    expectStdout(
        "module M6997; "
        "extension IEnumerable<int> public method Tag2(): int => 7; end; "
        "class C public static method Main(): int "
        "var a: Array<int> := [1, 2, 3]; "
        "var e: IEnumerable<int> := a; "
        "var f: () -> int := e.Tag2; "
        "Console.WriteLine(f()); "
        "return 0; end Main; end C;",
        "7\n");
}
