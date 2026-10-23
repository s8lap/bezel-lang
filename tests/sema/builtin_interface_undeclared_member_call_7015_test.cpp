// Sema — #7015 / stdlib §4.1, §4.12, §4.13 + language spec §27.6, §8.1 (v0.340).
//
// THE DEFECT. The CALL form of a member a builtin collection interface does
// not declare was ACCEPTED on the heads codegen's structural dispatch
// reaches:
//
//     var ro: IReadOnlyList<int> := a;
//     ro.Add(1);                    // compiled, rc 0 -- and it MUTATED the
//                                    // array behind the read-only list
//     var il: IList<int> := a;
//     Console.WriteLine(il.IndexOf(2));   // compiled, rc 0, printed 1
//
// stdlib §4.1 declares `IReadOnlyList<T>` as the indexer plus `Count` and
// `GetEnumerator` (inherited), and `IList<T>` as the indexer plus `Insert`
// inheriting the `ICollection<T>` members. `Add` and `IndexOf` are declared
// on neither head.
//
// THE RULE. Language spec §27.6 resolves a receiver-qualified reference "on
// the value exactly as the call `v.Scaled()` is", so the two spellings of one
// undeclared name must agree. #6997 made the BARE form agree with the call
// form; this fix makes the call form agree with the bare one, at the type
// level, so the name is refused before codegen's structural dispatch can
// reach it.
//
// THE SURFACE. The members an interface declares are the ones the stdlib
// documents for it, the union over its inherited ancestors (stdlib §4.1,
// §4.2, §4.12, §4.13). Four things are NOT rejected here, and each has a
// control row:
//   * the universal `Object` members (language spec §8.1: `GetType`, `Equals`,
//     `GetHashCode`, `ToString`, static `ReferenceEquals`);
//   * the stdlib §5 LINQ operator names (`il.Sum()`, `ro.Contains(3)`), which
//     resolve through the LINQ typing and not through the member surface --
//     the WHOLE documented §5 name set is exempt, not only the names the
//     shared 38-name table lists;
//   * a user extension that applies to the receiver (§26.5), which the call
//     resolves to exactly as the bare reference does (`il.Tag()`);
//   * the null-conditional spelling's own receivers;
//   * `Dispose` (CONTROL 12), whose membership in a head that does not extend
//     `IDisposable` the SPEC does not settle (stdlib §4.1 declares no
//     `Dispose` on the borrowing heads; language spec §8.7 makes `Dispose()` a
//     member of every non-static class) -- the call form is exempted and the
//     question is filed, rather than decided here.
//
// The four heads the #853 codegen backstop already refuses (the polymorphic
// collection interfaces and the owning list) are a SEPARATE fix (#7027): on
// those heads the call is already rejected, and the defect there is the
// #853 WORDING, which claims the member exists.
//
// ORACLE. `runAot`. The reject rows assert the diagnostic by needle and that
// it was a compile-time reject with a real location, so a crash or an
// unrelated compile error cannot pass. The control rows assert EXACT stdout,
// so a fix that over-rejects fails here.

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

// Assert the program COMPILES and runs to completion. Used for a control row
// whose OUTPUT is not what this fix is about (the row pins the acceptance, not
// the value).
void expectCompiles(const std::string& prog) {
    JitResult r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A `Main` over a borrowing `Array<int>` [1, 2, 3], so each row differs only in
// the one call under test.
std::string mainWith(const std::string& locals, const std::string& stmts) {
    return "module M7015; class C public static method Main(): int "
           "var a: Array<int> := [1, 2, 3]; " +
           locals + stmts + "return 0; end Main; end C;";
}

// The same, over an owning `OwningArray<Item>`, for the §4.13 family.
std::string owningMainWith(const std::string& locals, const std::string& stmts) {
    return "module M7015; public class Item public method Get(): int "
           "return 4; end Get; end Item; "
           "class C public static method Main(): int " +
           locals + stmts + "return 0; end Main; end C;";
}

}  // namespace

// ===========================================================================
// THE REJECTIONS — a call to a name the interface does not declare.
// ===========================================================================

// ROW 1 — the issue's own repro: `Add` on a READ-ONLY list. Before this fix
// the call compiled and mutated the array behind the read-only list.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ireadonlylist_call_add_is_rejected_spec_4_1) {
    expectRejected(
        mainWith("var ro: IReadOnlyList<int> := a; ", "ro.Add(1); "),
        "no member `Add` on `IReadOnlyList`");
}

// ROW 2 — the same head, the same defect, one name over: `Insert` is declared
// on `IList<T>` (stdlib §4.1) and on no other list head.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ireadonlylist_call_insert_is_rejected_spec_4_1) {
    expectRejected(
        mainWith("var ro: IReadOnlyList<int> := a; ", "ro.Insert(0, 1); "),
        "no member `Insert` on `IReadOnlyList`");
}

// ROW 3 — the issue's own repro, second name: `IndexOf` is declared on
// `Array<T>` (stdlib §4.4) and on no collection interface (§4.1).
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ilist_call_indexof_is_rejected_spec_4_1) {
    expectRejected(
        mainWith("var il: IList<int> := a; ",
                 "Console.WriteLine(il.IndexOf(2)); "),
        "no member `IndexOf` on `IList`");
}

// ROW 4 — a name no head declares at all. #6997's own ROW 12 pinned this
// call to the #2457 codegen backstop's wording; the type-level reject now
// answers it instead (that row is updated in the #6997 test file).
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ilist_call_nope_is_rejected_spec_4_1) {
    expectRejected(
        mainWith("var il: IList<int> := a; ",
                 "Console.WriteLine(il.Nope(5)); "),
        "no member `Nope` on `IList`");
}

// ROW 5 — the null-conditional CALL spelling reaches a separate sema block
// (the `?.` callee is a NullCondMemberExpr, and its typing goes through a
// SPECULATIVE twin that DISCARDS whatever it reports), so the reject is
// raised on the `?.` path itself.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ireadonlylist_null_conditional_call_add_is_rejected_spec_27_6) {
    expectRejected(
        mainWith("var ro: IReadOnlyList<int> := a; ", "ro?.Add(1); "),
        "no member `Add` on `IReadOnlyList`");
}

// ROW 6 — the same, on the `?.` spelling of a name no head declares.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ilist_null_conditional_call_nope_is_rejected_spec_27_6) {
    expectRejected(
        mainWith("var il: IList<int> := a; ", "il?.Nope(1); "),
        "no member `Nope` on `IList`");
}

// ROW 7 — the borrowing enumeration interface (stdlib §4.12) declares
// `GetEnumerator` only.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ienumerable_call_nope_is_rejected_spec_4_12) {
    expectRejected(
        mainWith("var e: IEnumerable<int> := a; ", "e.Nope(1); "),
        "no member `Nope` on `IEnumerable`");
}

// ROW 8 — the owning enumeration interface (stdlib §4.13) declares
// `GetOwningEnumerator` and `Dispose` only.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           iowningenumerable_call_nope_is_rejected_spec_4_13) {
    expectRejected(
        owningMainWith(
            "var oe: IOwningEnumerable<Item> := new OwningArray<Item>(); ",
            "oe.Nope(1); "),
        "no member `Nope` on `IOwningEnumerable`");
}

// ===========================================================================
// THE CONTROLS — the surfaces that must keep compiling, with exact stdout.
// ===========================================================================

// CONTROL 1 — a stdlib §5 operator on the read-only list: `Contains` is
// documented in §5's `where T: hashable` block, so the call is legitimate
// even though the head declares no `Contains`.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ireadonlylist_linq_contains_call_still_works_spec_5) {
    expectStdout(mainWith("var ro: IReadOnlyList<int> := a; ",
                          "Console.WriteLine(ro.Contains(3)); "),
                 "True\n");
}

// CONTROL 2 — a documented property read on the same interface.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ireadonlylist_count_read_still_prints_spec_4_1) {
    expectStdout(mainWith("var ro: IReadOnlyList<int> := a; ",
                          "Console.WriteLine(ro.Count); "),
                 "3\n");
}

// CONTROL 3 — the documented mutating call on `IList<T>` (inherited from
// `ICollection<T>`, stdlib §4.1) still mutates and still counts.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ilist_add_call_still_works_spec_4_1) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "il.Add(1); Console.WriteLine(il.Count); "),
                 "4\n");
}

// CONTROL 4 — `Insert` IS declared on `IList<T>` (stdlib §4.1), so the call
// is legitimate there — the reject is per head, not per name.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ilist_insert_call_still_works_spec_4_1) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "il.Insert(0, 1); Console.WriteLine(il.Count); "),
                 "4\n");
}

// CONTROL 5 — the documented `Contains` of `ICollection<T>` (stdlib §4.1).
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ilist_contains_call_still_works_spec_4_1) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "Console.WriteLine(il.Contains(2)); "),
                 "True\n");
}

// CONTROL 6 — a stdlib §5 operator on the list interface.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ilist_linq_sum_call_still_works_spec_5) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "Console.WriteLine(il.Sum()); "),
                 "6\n");
}

// CONTROL 7 — the stdlib §5 `Count()` operator (a METHOD, distinct from the
// `Count` property of the counted heads).
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ilist_linq_count_call_still_works_spec_5) {
    expectStdout(mainWith("var il: IList<int> := a; ",
                          "Console.WriteLine(il.Count()); "),
                 "3\n");
}

// CONTROL 8 — the same operator on the enumeration interface (stdlib §4.12).
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ienumerable_linq_sum_call_still_works_spec_5) {
    expectStdout(mainWith("var e: IEnumerable<int> := a; ",
                          "Console.WriteLine(e.Sum()); "),
                 "6\n");
}

// CONTROL 9 — a same-module user extension applies to the receiver (§26.5),
// so the call `il.Tag()` resolves to it and is not rejected.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           user_extension_call_on_ilist_still_works_spec_26_5) {
    expectStdout(
        "module M7015; "
        "extension IList<int> public method Tag(): int => 42; end; "
        "class C public static method Main(): int "
        "var a: Array<int> := [1, 2, 3]; "
        "var il: IList<int> := a; "
        "Console.WriteLine(il.Tag()); "
        "return 0; end Main; end C;",
        "42\n");
}

// CONTROL 10 — the `?.` spelling of the same user extension.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           user_extension_null_conditional_call_on_ilist_still_works_spec_26_5) {
    expectStdout(
        "module M7015; "
        "extension IList<int> public method Tag(): int => 42; end; "
        "class C public static method Main(): int "
        "var a: Array<int> := [1, 2, 3]; "
        "var il: IList<int> := a; "
        "var t: int? := il?.Tag(); "
        "Console.WriteLine(1); "
        "return 0; end Main; end C;",
        "1\n");
}

// CONTROL 11 — the documented property read through the `?.` spelling. The
// `?.` property read on a list interface does not lower to a value (the
// pre-existing #3079 degradation, unrelated to this fix), so this row pins
// the ACCEPTANCE only: the `?.` call-form reject must not fire on a name the
// head declares.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ireadonlylist_null_conditional_count_read_still_compiles_spec_4_1) {
    expectCompiles(mainWith("var ro: IReadOnlyList<int> := a; ",
                            "Console.WriteLine(ro?.Count); "));
}

// CONTROL 12 — `Dispose` on a borrowing collection interface head. The
// compiler routes the call to the concrete collection's disposal and TEN
// shipped rows pin that it COMPILES and runs (`codegen_iface_head_dispose_2970`,
// `codegen_iface_head_dispose_3026`, `codegen_iface_head_raw_user_class_2975`,
// `codegen_nullcond_dispose_collections_4221`,
// `iowningenumerable_explicit_dispose_6553`,
// `named_local_property_read_source_abandonment_3605`). The `Dispose` call is
// NOT a member-surface question there: the compiler TYPES it and applies
// language spec §28.6's owned-receiver rule, which admits this shape (the
// local adopts the `new Array<int>(2)` temporary's ownership) and refuses a
// borrow. So this fix must not pre-empt that check with a "no member"
// diagnostic -- hence the exemption, and this row pins it.
//
// The SPEC does not settle whether `Dispose` is a member of a head that does
// not extend `IDisposable` (stdlib §4.1 declares no `Dispose` on the
// borrowing heads; language spec §8.7 makes `Dispose()` a member of every
// non-static class), so the question is filed rather than decided here; the
// BARE form's #6997 rejection ("no member `Dispose` on `IList`", the #6997
// ROW 4) is left exactly as it shipped.
BEZEL_TEST(builtin_interface_undeclared_member_call_7015,
           ilist_dispose_call_still_compiles_spec_8_7) {
    expectCompiles("module M7015; class C public static method Main(): int "
                   "var il: IList<int> := new Array<int>(2); "
                   "il.Dispose(); return 0; end Main; end C;");
}
