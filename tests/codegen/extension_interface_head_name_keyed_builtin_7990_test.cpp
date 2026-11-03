// Regression tests for #7990 -- a name-keyed built-in collection arm (Peek /
// Push / Enqueue / ...) swallowed a user extension on a COLLECTION INTERFACE
// receiver.
//
// WHAT THE SPEC REQUIRES.  Language spec §26.5 rule 1 (v0.350): "If `x`'s
// declared type (or any base type) *declares* any instance member named `Foo`
// -- of any arity, applicability, or accessibility -- instance resolution is
// committed and extension blocks are **never consulted** for that name on
// that receiver."  Stdlib §4.1 declares `IReadOnlyList<T>` and `IList<T>` with
// `Count`, the indexer, `GetEnumerator` and (for `IList<T>`) `IsReadOnly`,
// `Add`, `Clear`, `Contains`, `Remove` and `Insert` -- and NO `Peek`, `Push`
// or `Enqueue`.  So rule 1 does not commit, rule 2 selects the extension, and
// §22.6 binds the call to it.
//
// THE DEFECT.  #7946's fix taught the codegen's name-keyed arms to decline a
// call that sema bound to a user extension, but it asked the CONCRETE head's
// declared surface -- and the collection INTERFACE heads have no row there.
// The borrowing LIST interfaces are array-backed at runtime (their value is
// the same `bz_array_t*` the concrete `Array<T>` carries), so the arms
// reached them anyway and lowered the call as the built-in member: the
// extension's 7 became the built-in's out-of-range array read, 0.
//
// ORACLE: exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {
std::string prog(const std::string& ext, const std::string& decl,
                 const std::string& body) {
    return "module R; " + ext +
           " class Program public static method Main(): int " + decl + body +
           " return 0; end Main; end Program;";
}

// The receiver: an `Array<int>` behind the interface head under test.
std::string ifaceDecl(const std::string& head) {
    return " var a0: Array<int> := [1,2,3]; var a: " + head +
           "<int> := a0; ";
}
}  // namespace

// ---- the interface heads that were wrong ----------------------------------

// §26.5 rule 2 (v0.350): `IList<T>` declares no `Peek`; the extension is
// selected and its 7 is the answer.
BEZEL_TEST(extension_interface_head_name_keyed_builtin_7990, ilist_peek) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension IList<int> public method Peek(n: int): int => 7; end;",
        ifaceDecl("IList"), "Console.WriteLine(a.Peek(10));")), "7");
}

// §26.5 rule 2 (v0.350): `IList<T>` declares no `Push`.
BEZEL_TEST(extension_interface_head_name_keyed_builtin_7990, ilist_push) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension IList<int> public method Push(n: int): int => 7; end;",
        ifaceDecl("IList"), "Console.WriteLine(a.Push(10));")), "7");
}

// §26.5 rule 2 (v0.350): `IList<T>` declares no `Enqueue`.
BEZEL_TEST(extension_interface_head_name_keyed_builtin_7990, ilist_enqueue) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension IList<int> public method Enqueue(n: int): int => 7; end;",
        ifaceDecl("IList"), "Console.WriteLine(a.Enqueue(10));")), "7");
}

// §26.5 rule 2 (v0.350): `IReadOnlyList<T>` declares no `Peek` either.
BEZEL_TEST(extension_interface_head_name_keyed_builtin_7990, ireadonlylist_peek) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension IReadOnlyList<int> public method Peek(n: int): int => 7; end;",
        ifaceDecl("IReadOnlyList"), "Console.WriteLine(a.Peek(10));")), "7");
}

// §26.5 rule 2 (v0.350): `IReadOnlyList<T>` declares no `Push`.
BEZEL_TEST(extension_interface_head_name_keyed_builtin_7990, ireadonlylist_push) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension IReadOnlyList<int> public method Push(n: int): int => 7; end;",
        ifaceDecl("IReadOnlyList"), "Console.WriteLine(a.Push(10));")), "7");
}

// ---- controls --------------------------------------------------------------

// The concrete head: `Array<T>` declares no `Peek` either (#7946's row).
BEZEL_TEST(extension_interface_head_name_keyed_builtin_7990, array_peek) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Peek(n: int): int => 7; end;",
        " var a: Array<int> := [1,2,3]; ", "Console.WriteLine(a.Peek(10));")), "7");
}

// `ICollection<T>` already dispatched the extension (the array bridge does not
// reach it); it must keep doing so.
BEZEL_TEST(extension_interface_head_name_keyed_builtin_7990, icollection_peek) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension ICollection<int> public method Peek(n: int): int => 7; end;",
        ifaceDecl("ICollection"), "Console.WriteLine(a.Peek(10));")), "7");
}

// `IEnumerable<T>` likewise.
BEZEL_TEST(extension_interface_head_name_keyed_builtin_7990, ienumerable_peek) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension IEnumerable<int> public method Peek(n: int): int => 7; end;",
        ifaceDecl("IEnumerable"), "Console.WriteLine(a.Peek(10));")), "7");
}

// The members the interface DOES declare keep the built-in arm (§26.5 rule 1
// commits them): `Count`, `Contains`, `Add` and `Remove` are stdlib §4.1's
// declared surface, so they must still answer.
BEZEL_TEST(extension_interface_head_name_keyed_builtin_7990, ilist_declared_members_keep_the_builtin) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "",
        ifaceDecl("IList"),
        "Console.WriteLine(a.Count);"
        "Console.WriteLine(a.Contains(2));"
        "a.Add(4);"
        "Console.WriteLine(a.Count);"
        "Console.WriteLine(a.Remove(2));"
        "Console.WriteLine(a.Count);")),
        "3\nTrue\n4\nTrue\n3");
}