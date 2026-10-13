// fix #6125 (stdlib §4.1, §1 "Count vs Length", v0.338; spec §7.5): the
// borrowing list and read-only interfaces accepted `.Length` / `?.Length`
// although the stdlib declares only `Count` on them.
//
// stdlib §4.1 (quoted from docs/Bezel_Standard_Library.md at a5bed881):
//   public interface IReadOnlyCollection<out T>: IEnumerable<T>
//     pure readonly property Count: int;
//   public interface IReadOnlyList<out T>: IReadOnlyCollection<T>
//     readonly property [i: int]: T;
//   public interface IList<T>: IReadOnlyList<T>, ICollection<T>
//     property [i: int]: T;
//     method Insert(index: int, item: T);
// None of the three names a `Length`; §1's "Count vs Length" row says "A type
// declares one or the other, never both". The rejection follows the #6081
// convention for `ICollection<T>` (an implementation convention, same
// convention, pending the owner's ruling on which member a type lacks).
//
// Rows (rejected, named member in the message):
//   R1  `IList<int>?.Length`                 (§4.1)
//   R2  `IReadOnlyList<int>.Length`          (§4.1, dot form)
//   R3  `IReadOnlyCollection<int>?.Length`   (§4.1)
//   R4  `IList<int>.Length` as an argument   (§4.1, argument position)
// Controls (accepted, run, stdout asserted):
//   C1  `IList<int>.Count` prints 3          (§4.1, declared member)
//   C2  `IReadOnlyList<int>?.Count` is accepted and runs (§4.1; stdout not
//       asserted, #3079)
//   C3  a user-declared `IList<T>` with its own `Length` prints 5 (the check
//       is gated on the built-in head, `!declaration`)
// Call spellings (`c.Length()`) are not part of this change; they remain as
// #6081 left them.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── R1 — `?.Length` on IList<T>? (§4.1: only Count).
BEZEL_TEST(sema_member_set_residual_6125,
           ilist_nullcond_length_rejected_stdlib_4_1) {
    auto r = runAot(
        "module M6125a; "
        "class Program public static method Main(): int "
        "  var c: IList<int>? := new Array<int>(); "
        "  var x := c?.Length; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Length` on `IList<T>`: use `Count`"));
}

// ── R2 — `.Length` on IReadOnlyList<T> (§4.1: only the indexer and Count).
BEZEL_TEST(sema_member_set_residual_6125,
           ireadonlylist_dot_length_rejected_stdlib_4_1) {
    auto r = runAot(
        "module M6125b; "
        "class Program public static method Main(): int "
        "  var c: IReadOnlyList<int> := new Array<int>(); "
        "  var x := c.Length; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Length` on `IReadOnlyList<T>`: use `Count`"));
}

// ── R3 — `?.Length` on IReadOnlyCollection<T>? (§4.1: Count only).
BEZEL_TEST(sema_member_set_residual_6125,
           ireadonlycollection_nullcond_length_rejected_stdlib_4_1) {
    auto r = runAot(
        "module M6125c; "
        "class Program public static method Main(): int "
        "  var c: IReadOnlyCollection<int>? := new Array<int>(); "
        "  var x := c?.Length; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Length` on `IReadOnlyCollection<T>`: use `Count`"));
}

// ── R4 — `.Length` on IList<T> in argument position (the callee-slot gate
// covers an argument read, as for the #6081 rows).
BEZEL_TEST(sema_member_set_residual_6125,
           ilist_dot_length_argument_rejected_stdlib_4_1) {
    auto r = runAot(
        "module M6125d; "
        "class Program public static method Main(): int "
        "  var c: IList<int> := new Array<int>(); "
        "  Console.WriteLine(c.Length); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Length` on `IList<T>`: use `Count`"));
}

// ── C1 — the declared member: IList<int>.Count runs (§4.1).
BEZEL_TEST(sema_member_set_residual_6125,
           ilist_dot_count_runs_stdlib_4_1) {
    auto r = runAot(
        "module M6125e; "
        "class Program public static method Main(): int "
        "  var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var c: IList<int> := a; "
        "  Console.WriteLine(c.Count); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// ── C2 — the declared member: IReadOnlyList<int>?.Count is accepted and the
// program runs (§4.1). Its stdout is not asserted: a `?.` read on a nullable
// interface head emits nothing in Console.WriteLine (the #3079 codegen gap,
// pre-existing on the base, not pinned here).
BEZEL_TEST(sema_member_set_residual_6125,
           ireadonlylist_nullcond_count_accepted_stdlib_4_1) {
    auto r = runAot(
        "module M6125f; "
        "class Program public static method Main(): int "
        "  var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var c: IReadOnlyList<int>? := a; "
        "  var x := c?.Count; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── C3 — a user-declared class named IList<T> with its own Length: the check
// is gated on the built-in head, so the user member is accepted and runs.
BEZEL_TEST(sema_member_set_residual_6125,
           user_ilist_own_length_accepted_runs) {
    auto r = runAot(
        "module M6125g; "
        "class IList<T> public property Length: int => 5; end IList; "
        "class Program public static method Main(): int "
        "  var l := new IList<int>(); "
        "  Console.WriteLine(l.Length); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}
