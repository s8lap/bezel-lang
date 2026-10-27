// Regression tests for #6159 — the built-in `Length` / `Count` intrinsic
// answered a call that was bound to the USER'S extension, so the user's code
// was silently DROPPED (the program printed the built-in's number instead of
// the extension's).
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   §26.5 (v0.345), rule 1 — "Instance members win — by name.  If `x`'s
//   declared type (or any base type) *declares* any instance member named
//   `Foo` — of any arity, applicability, or accessibility — instance
//   resolution is committed and extension blocks are **never consulted** for
//   that name on that receiver."
//
//   §26.5, rule 2 — extension blocks are searched on x's type, its base
//   types, or its implemented interfaces.
//
//   §22.6 — a call binds to the candidate that was selected.
//
//   So for a receiver whose DECLARED surface does NOT contain the member
//   name, rule 1 does not commit, rule 2 admits the extension blocks, and the
//   user's extension is the SELECTED candidate.  A built-in intrinsic that
//   recognises its target by NAME answers such a call anyway, and the user's
//   extension never runs.
//
// THE DECLared SURFACE (stdlib, v0.345).
//
//   §3.1  `StringBuilder` declares `Length` and `Capacity` — NOT `Count`.
//   §4.1  `IReadOnlyCollection<out T>` declares `Count: int`;
//         `IReadOnlyList<T>: IReadOnlyCollection<T>`,
//         `ICollection<T>: IReadOnlyCollection<T>`,
//         `IList<T>: IReadOnlyList<T>, ICollection<T>`,
//         `IOwningCollection<T>: IReadOnlyCollection<T>`,
//         `IOwningList<T>: IReadOnlyList<T>, IOwningCollection<T>` — so all
//         SIX collection interfaces INHERIT `Count`, and NONE declares
//         `Length`.
//   §4.4  `Array<T>` declares `Count` and `Capacity` — NOT `Length`.
//
//   The stdlib's convention rows say the same: "Block-by-name" —
//   "extension resolution commits to instance members by name, inherited ones
//   included"; "Count vs Length" — "A type declares one or the other, never
//   both".
//
// THE FIX.  `Emitter::tryLowerCollectionInstanceCall` recognises the built-in
// by the RESOLVED CALLEE SYMBOL (sema stamps the selected extension's
// MethodSymbol on the member-access callee) plus the receiver's DECLARED
// SURFACE — the same "the receiver's SYMBOL, not its NAME" discipline #7217 /
// #6845 applied to the receiver's own type.
//
// THE ORACLE.  A dropped call prints the WRONG VALUE, so the oracle is the
// program's EXACT stdout (never the exit code: wrong code that happens not to
// crash exits 0 too).  The mirror rows are load-bearing — they pin the
// built-in's answer where rule 1 DOES commit, so a fix that simply made the
// built-in decline every `Length`/`Count` would fail them.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// The user's extension and the program that calls it, with the extension
// declared first (the declaration order is irrelevant to the defect; it only
// keeps the program readable).
std::string sbProgram(const std::string& ext, const std::string& body) {
    return "module R; " + ext +
           " class Program public static method Main(): int "
           "  var sb := new StringBuilder(); "
           "  sb.Append(\"abc\"); " + body +
           "  return 0; "
           " end Main; end Program;";
}

std::string listProgram(const std::string& ext, const std::string& body) {
    return "module R; " + ext +
           " class Program public static method Main(): int "
           "  var a: Array<int> := [1,2,3]; "
           "  var c: IList<int> := a; " + body +
           "  return 0; "
           " end Main; end Program;";
}

}  // namespace

// ── the DEFECT: the receiver's surface does NOT declare the name ────────────

// per §26.5 rule 2 (v0.345) + stdlib §3.1: `StringBuilder` declares
// `Length`/`Capacity`, NOT `Count`, so rule 1 does not commit and the user's
// extension is the selected candidate — the program must print the
// EXTENSION's 77, not the built-in's content length 3.
BEZEL_TEST(extension_receiver_symbol_6159, sb_count_user_extension_wins) {
    // per §26.5 rule 2 (v0.345); stdlib §3.1
    BEZEL_EXPECT_EQ(runAotOut(sbProgram(
        "extension StringBuilder public method Count(): int => 77; end;",
        "var i: int := sb.Count(); Console.WriteLine(i);")),
        "77");
}

// The extension's RETURN TYPE is the selected candidate's type too: a
// `string`-returning `Count` must print that string.  Before the fix the
// built-in answered with an `int` and the program printed the number.
BEZEL_TEST(extension_receiver_symbol_6159, sb_count_user_extension_return_type) {
    // per §22.6 (v0.345) + stdlib §3.1
    BEZEL_EXPECT_EQ(runAotOut(sbProgram(
        "extension StringBuilder public method Count(): string => \"EXTSBC\"; end;",
        "var s: string := sb.Count(); Console.WriteLine(s);")),
        "EXTSBC");
}

// The built-in arm claimed `Count` at ANY arity (its arm has no argument
// check), so a one-argument extension was dropped as well.
BEZEL_TEST(extension_receiver_symbol_6159, sb_count_with_argument_user_extension_wins) {
    // per §26.5 rule 2 (v0.345); stdlib §3.1
    BEZEL_EXPECT_EQ(runAotOut(sbProgram(
        "extension StringBuilder public method Count(x: int): int => 77 + x; end;",
        "var i: int := sb.Count(5); Console.WriteLine(i);")),
        "82");
}

// per §26.5 rule 2 (v0.345) + stdlib §4.1: `IList<T>` declares NO `Length`
// (it inherits `Count`), so the extension block is consulted and the user's
// extension is the selected candidate.
BEZEL_TEST(extension_receiver_symbol_6159, ilist_length_user_extension_wins) {
    // per §26.5 rule 2 (v0.345); stdlib §4.1
    BEZEL_EXPECT_EQ(runAotOut(listProgram(
        "extension IList<int> public method Length(): int => 77; end;",
        "var i: int := c.Length(); Console.WriteLine(i);")),
        "77");
}

// The same shape with an INTERFACE extension block: `IEnumerable<T>` is an
// implemented interface of `IList<T>`, which rule 2 admits.
BEZEL_TEST(extension_receiver_symbol_6159, ilist_length_ienumerable_extension_wins) {
    // per §26.5 rule 2 (v0.345); stdlib §4.1
    BEZEL_EXPECT_EQ(runAotOut(listProgram(
        "extension IEnumerable<int> public method Length(): int => 77; end;",
        "var i: int := c.Length(); Console.WriteLine(i);")),
        "77");
}

// ── the MIRROR: the receiver's surface DOES declare the name ────────────────

// per §26.5 rule 1 (v0.345) + stdlib §3.1: `StringBuilder` DECLARES
// `Length`, so instance resolution is committed and the extension blocks are
// never consulted — the built-in answers with the content length (3).
BEZEL_TEST(extension_receiver_symbol_6159, sb_length_declared_member_wins) {
    // per §26.5 rule 1 (v0.345); stdlib §3.1
    BEZEL_EXPECT_EQ(runAotOut(sbProgram(
        "extension StringBuilder public method Length(): int => 77; end;",
        "var i: int := sb.Length(); Console.WriteLine(i);")),
        "3");
}

// per §26.5 rule 1 (v0.345) + stdlib §4.1: `IList<T>` INHERITS `Count` from
// `IReadOnlyCollection<T>`, and an inherited member commits rule 1 too (the
// stdlib's "Block-by-name" convention: "extension resolution commits to
// instance members by name, inherited ones included") — the built-in answers.
BEZEL_TEST(extension_receiver_symbol_6159, ilist_count_inherited_member_wins) {
    // per §26.5 rule 1 (v0.345); stdlib §4.1
    BEZEL_EXPECT_EQ(runAotOut(listProgram(
        "extension IList<int> public method Count(): int => 77; end;",
        "var i: int := c.Count(); Console.WriteLine(i);")),
        "3");
}

// per §26.5 rule 1 (v0.345) + stdlib §4.4: `Array<T>` DECLARES `Count`, so
// the extension block is never consulted — the built-in answers.
BEZEL_TEST(extension_receiver_symbol_6159, array_count_declared_member_wins) {
    // per §26.5 rule 1 (v0.345); stdlib §4.4
    BEZEL_EXPECT_EQ(runAotOut(
        "module R; "
        "extension Array<int> public method Count(): int => 77; end; "
        "class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var i: int := a.Count(); Console.WriteLine(i); "
        "  return 0; "
        "end Main; end Program;"),
        "3");
}

// ── controls: no extension at all ───────────────────────────────────────────

// With NO user extension the built-in answers every one of these — the fix
// must not change a program that has no extension in it.
BEZEL_TEST(extension_receiver_symbol_6159, no_extension_builtin_still_answers) {
    // per stdlib §3.1 / §4.1 / §4.4
    BEZEL_EXPECT_EQ(runAotOut(
        "module R; "
        "class Program public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var c: IList<int> := a; "
        "  var sb := new StringBuilder(); sb.Append(\"abc\"); "
        "  Console.WriteLine(a.Count()); "
        "  Console.WriteLine(c.Count()); "
        "  Console.WriteLine(sb.Length()); "
        "  return 0; "
        "end Main; end Program;"),
        "3\n3\n3");
}

// An extension whose name no built-in arm claims is untouched by the fix.
BEZEL_TEST(extension_receiver_symbol_6159, unrelated_extension_unaffected) {
    // per §26.5 rule 2 (v0.345)
    BEZEL_EXPECT_EQ(runAotOut(sbProgram(
        "extension StringBuilder public method Twice(): int => 77; end;",
        "var i: int := sb.Twice(); Console.WriteLine(i);")),
        "77");
}
