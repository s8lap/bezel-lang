// Sema — #7019 / stdlib §5 + language spec §27.6 (v0.340).
//
// THE DEFECT. The #6784 arm that rejects a BARE reference to a stdlib §5
// operator tested the name against the shared 38-name table
// (`kBuiltinCollectionLinqOperatorNames`, src/common/BuiltinRegistry.cpp).
// That table answers a narrower question -- "is this a builtin-collection
// LINQ operator the compiler dispatches structurally" -- and it MISSES two
// names stdlib §5 documents:
//
//     var a: Array<int> := [1, 2, 3];
//     var il: IList<int> := a;
//     var f := il.ElementAtOrNull;      // and `il.Reverse`
//
// So the reference was left UNTYPE D, codegen degraded the local to a zero
// function value (the #2434 degradation), and calling it crashed -- exactly
// the failure #6784 was filed for. Instead it got the GENERIC "no member"
// wording from fix #6997's arm, which says the name is not part of the
// interface's documented surface: true, but the wrong reason, because the
// name IS a documented stdlib §5 member (its "not on that surface" sentence
// even offers the §5 escape as a possibility).
//
// THE RULE. The name test must be stdlib §5's OWN list. The list is read
// from `kStdlibLinqOverloads` (src/sema/LinqStdlibOverloads.hpp) -- the same
// table the LINQ arity gate reads (fix #3726) -- so it cannot drift from
// the stdlib again. Both spellings are covered: the `.` reference
// (language spec §27.6, "the member resolved on the value exactly as the
// call `v.Scaled()` is") and the `?.` spelling, which reaches a separate
// sema block.
//
// WHAT IS NOT TAKEN. A name the receiver's head DECLARES keeps its own
// resolution, and resolution on the receiver is committed before the
// operator lookup (language spec §26.5 rule 1):
//   * `il.Count` is the `Count` PROPERTY of stdlib §4.1 -- a property read,
//     not the §5 `Count()` operator;
//   * `il.Contains(2)` is `ICollection<T>.Contains` (stdlib §4.1), the
//     instance member, not the §5 hashable operator.
// Both have control rows. The concrete and span families keep the shared
// table's set: their declared surface is not sema's to read, and their own
// per-family arms answer the bare reference there.
//
// The CALL form is unaffected and is pinned too: `il.Reverse()` is a
// legitimate §5 call that compiles and runs, while
// `il.ElementAtOrNull(1)` reaches the #2457 codegen backstop (the compiler
// has no dispatch arm for that operator) -- neither is a member-surface
// question.
//
// ORACLE. `runAot`. The reject rows assert the diagnostic by needle and that
// it was a compile-time reject with a real location, so a crash or an
// unrelated compile error cannot pass. The control rows assert EXACT stdout.

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
// in the one reference under test.
std::string mainWith(const std::string& locals, const std::string& stmts) {
    return "module M7019; class C public static method Main(): int "
           "var a: Array<int> := [1, 2, 3]; " +
           locals + stmts + "return 0; end Main; end C;";
}

}  // namespace

// ===========================================================================
// THE REJECTIONS — a bare reference to a documented stdlib §5 operator.
// ===========================================================================

// ROW 1 — the issue's own repro. `ElementAtOrNull` is documented in stdlib
// §5; before this fix the reference was ACCEPTED, degraded to a zero
// function value (#2434) and crashed when called.
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ilist_bare_elementatornull_is_rejected_spec_5) {
    expectRejected(
        mainWith("var il: IList<int> := a; ", "var f := il.ElementAtOrNull; "),
        "a bare reference to `ElementAtOrNull` (language spec \xC2\xA7""27.6) "
        "is not yet supported");
}

// ROW 2 — the second missing name. `Reverse` is documented in stdlib §5 and
// is distinct from `Array<T>.Reverse` (stdlib §4.4), the in-place reverse on
// the CONCRETE type only.
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ilist_bare_reverse_is_rejected_spec_5) {
    expectRejected(
        mainWith("var il: IList<int> := a; ", "var f := il.Reverse; "),
        "a bare reference to `Reverse` (language spec \xC2\xA7""27.6) "
        "is not yet supported");
}

// ROW 3 — the enumeration interface (stdlib §4.12) declares
// `GetEnumerator` only, so the §5 operator is the only thing the name can
// mean there.
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ienumerable_bare_elementatornull_is_rejected_spec_5) {
    expectRejected(
        mainWith("var e: IEnumerable<int> := a; ",
                 "var f := e.ElementAtOrNull; "),
        "a bare reference to `ElementAtOrNull` (language spec \xC2\xA7""27.6) "
        "is not yet supported");
}

// ROW 4 — the read-only list head (stdlib §4.1).
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ireadonlylist_bare_reverse_is_rejected_spec_5) {
    expectRejected(
        mainWith("var ro: IReadOnlyList<int> := a; ", "var f := ro.Reverse; "),
        "a bare reference to `Reverse` (language spec \xC2\xA7""27.6) "
        "is not yet supported");
}

// ROW 5 — `Contains` is documented in stdlib §5's `where T: hashable`
// block, and `IReadOnlyList<T>` declares NO `Contains` (stdlib §4.1 gives it
// the indexer, `Count` and `GetEnumerator`), so on that head the §5
// operator is what the name denotes -- and it gets the §5 wording.
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ireadonlylist_bare_contains_is_rejected_spec_5) {
    expectRejected(
        mainWith("var ro: IReadOnlyList<int> := a; ", "var f := ro.Contains; "),
        "a bare reference to `Contains` (language spec \xC2\xA7""27.6) "
        "is not yet supported");
}

// ROW 6 — the `?.` spelling reaches a separate sema block (the `?.` callee
// is a NullCondMemberExpr), and the two spellings of one name must agree
// (language spec §27.6). Before this fix the R34 arm answered it with the
// generic wording, because that arm ran first on this path.
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ilist_null_conditional_bare_elementatornull_is_rejected_spec_27_6) {
    expectRejected(
        mainWith("var il: IList<int> := a; ", "var f := il?.ElementAtOrNull; "),
        "a bare reference to `ElementAtOrNull` (language spec \xC2\xA7""27.6) "
        "is not yet supported");
}

// ===========================================================================
// THE CONTROLS — what must NOT change.
// ===========================================================================

// CONTROL 1 — `Count` is a documented §5 name AND the documented PROPERTY of
// the counted heads (stdlib §4.1). On a head that declares it, the property
// read wins (language spec §26.5 rule 1) and the reference is not a §5
// operator reference at all.
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ilist_count_property_read_still_prints_spec_4_1) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "Console.WriteLine(il.Count); "),
                 "3\n");
}

// CONTROL 2 — the same for the CALL form of `Contains`: `ICollection<T>`
// declares it (stdlib §4.1), so the instance method resolves and the §5
// operator does not.
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ilist_contains_call_still_works_spec_4_1) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "Console.WriteLine(il.Contains(2)); "),
                 "True\n");
}

// CONTROL 3 — the §5 `Count()` OPERATOR spelled as a call is legitimate on
// every head, including the ones that declare the property (§5 names the
// method; §4.1 names the property).
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ilist_linq_count_call_still_works_spec_5) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "Console.WriteLine(il.Count()); "),
                 "3\n");
}

// CONTROL 4 — an ordinary §5 operator call.
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ilist_linq_sum_call_still_works_spec_5) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "Console.WriteLine(il.Sum()); "),
                 "6\n");
}

// CONTROL 5 — `Reverse` spelled as a CALL is a legitimate §5 call and it
// compiles and runs: the reject is about the bare reference only (language
// spec §27.6 forms no function value for a structurally dispatched
// operator). The row prints a constant rather than the call's RESULT on
// purpose: consuming a lazy LINQ result through an interface receiver does
// not lower today (the pre-existing #2434 / #3079 degradations, a codegen
// gap and not a member-surface question — see the handback), and pinning
// that would make this row fail on an unrelated fix.
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ilist_linq_reverse_call_still_works_spec_5) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "il.Reverse(); Console.WriteLine(9); "),
                 "9\n");
}

// CONTROL 6 — a name stdlib §5 does NOT document keeps the GENERIC wording.
// `ToList` is not in stdlib §5 (it is `ToArray` there), so the §5 name set
// must not claim it. This row is what stops the fix from turning the arm
// into a blanket "every unknown member is a §5 operator" rejection.
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ilist_bare_tolist_keeps_the_generic_wording_spec_4_1) {
    expectRejected(
        mainWith("var il: IList<int> := a; ", "var f := il.ToList; "),
        "no member `ToList` on `IList`");
}

// CONTROL 7 — a §5 name the shared table ALREADY listed keeps the wording it
// shipped with (no wording regression on the existing set).
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ilist_bare_sum_still_gets_the_6784_wording_spec_5) {
    expectRejected(
        mainWith("var il: IList<int> := a; ", "var f := il.Sum; "),
        "a bare reference to `Sum` (language spec \xC2\xA7""27.6) "
        "is not yet supported");
}

// CONTROL 8 — a user extension that applies to the receiver (language spec
// §26.5) is what the bare reference resolves to, so it is not rejected.
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           user_extension_bare_group_on_ilist_still_works_spec_26_5) {
    expectStdout(
        "module M7019; "
        "extension IList<int> public method Tag(): int => 42; end; "
        "class C public static method Main(): int "
        "var a: Array<int> := [1, 2, 3]; "
        "var il: IList<int> := a; "
        "var f: () -> int := il.Tag; "
        "Console.WriteLine(f()); "
        "return 0; end Main; end C;",
        "42\n");
}

// CONTROL 9 — the CALL form of the name this fix adds (`ElementAtOrNull`) is
// NOT taken for an undeclared member: it is a documented §5 call, and it
// reaches codegen's #2457 backstop there because the compiler has no
// dispatch arm for that operator yet. The row pins that the "no member"
// wording does NOT appear.
BEZEL_TEST(stdlib_linq_bare_reference_7019,
           ilist_elementatornull_call_is_not_a_no_member_reject_spec_5) {
    JitResult r = runAot(mainWith("var il: IList<int> := a; ",
                                  "Console.WriteLine(il.ElementAtOrNull(1)); "));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "no member"));
}