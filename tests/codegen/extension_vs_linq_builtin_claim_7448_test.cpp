// Regression tests for #7448 -- the collection built-in arm
// (Emitter::tryLowerCollectionInstanceCall) claimed LINQ operator names (Sum,
// Max, ...) by NAME, so a user extension that sema had bound as the most
// specific candidate was silently dropped (the program printed the built-in's
// 6 instead of the extension's 77).
//
// WHAT THE SPEC REQUIRES.  Language spec §26.5 (v0.350) rules 2 and 3 and
// §26.5.1: for a receiver of type `Array<int>`, `extension Array<T>` (or
// `Array<int>`) is more specific than the stdlib's `extension IEnumerable<T>`
// (stdlib §5), so the user's extension is the selected candidate and §22.6
// binds the call to it.  An `extension IEnumerable<int>` is NOT more specific
// than the stdlib one (rule 4: ambiguity), so it is not tested as a winner.
//
// ORACLE: exact stdout (a dropped call prints a wrong value and exits 0).

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
const char* kArr = " var a: Array<int> := [1,2,3]; ";
const char* kIList = " var a: Array<int> := [1,2,3]; var c: IList<int> := a; ";
}  // namespace

// §26.5 rules 2-3, §26.5.1 (v0.350): the issue's repro.
BEZEL_TEST(extension_vs_linq_7448, array_sum_user_extension_wins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Sum(): int => 77; end;", kArr,
        "Console.WriteLine(a.Sum());")), "77");
}

// §26.5.1 (v0.350): same with Max.
BEZEL_TEST(extension_vs_linq_7448, array_max_user_extension_wins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Max(): int => 77; end;", kArr,
        "Console.WriteLine(a.Max());")), "77");
}

// §22.6 (v0.350): the selected candidate's return type is the call's type.
BEZEL_TEST(extension_vs_linq_7448, array_sum_string_return) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Sum(): string => \"EXT\"; end;",
        kArr, "var s: string := a.Sum(); Console.WriteLine(s);")), "EXT");
}

// §26.5 rule 2 (v0.350): IList<int> receiver, extension on IList<int>.
BEZEL_TEST(extension_vs_linq_7448, ilist_sum_user_extension_wins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension IList<int> public method Sum(): int => 77; end;", kIList,
        "Console.WriteLine(c.Sum());")), "77");
}

// §26.5 rule 2 (v0.350): OwningArray<int> receiver.
BEZEL_TEST(extension_vs_linq_7448, owning_array_sum_user_extension_wins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "class Item public method Id(): int => 1; end Item; extension OwningArray<T> where T: class public method Sum(): int => 77; end;",
        " using var a := new OwningArray<Item>(); a.Add(new Item()); ",
        "Console.WriteLine(a.Sum());")), "77");
}

// Other operators on the same receiver keep the built-in (user ext only
// replaces Sum).  `Count` is not used here: stdlib §4.4 declares it as a
// PROPERTY of `Array<T>`, so it is not one of the §5-only operators.
BEZEL_TEST(extension_vs_linq_7448, other_operators_still_builtin) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Sum(): int => 77; end;", kArr,
        "Console.WriteLine(a.Sum()); Console.WriteLine(a.Max());"
        "Console.WriteLine(a.Min());")), "77\n3\n1");
}

// ---------------------------------------------------------------------------
// §26.5 rule 1 (v0.350): "If `x`'s declared type (or any base type) declares
// any instance member named `Foo` — of any arity, applicability, or
// accessibility — instance resolution is committed and extension blocks are
// never consulted for that name on that receiver."  The built-in collection
// heads DECLARE these names (stdlib §4.4 Array<T>: Contains / IndexOf /
// Reverse / Sort; §4.10 Set<T>: Add, from ICollection<T>; §4.8
// Dictionary<K, V>: ContainsKey), so a same-named user extension is never
// the callee and the built-in answers.  A decline keyed on "sema bound an
// extension" alone dropped the instance member for every one of them.
// ---------------------------------------------------------------------------

BEZEL_TEST(extension_vs_linq_7448, rule1_array_contains_instance_member_wins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Contains(x: int): bool => false; end;",
        kArr, "Console.WriteLine(a.Contains(2));")), "True");
}

BEZEL_TEST(extension_vs_linq_7448, rule1_array_indexof_instance_member_wins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method IndexOf(x: int): int => 99; end;",
        kArr, "Console.WriteLine(a.IndexOf(2));")), "1");
}

BEZEL_TEST(extension_vs_linq_7448, rule1_array_reverse_instance_member_wins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Reverse() end Reverse; end;",
        kArr, "a.Reverse(); Console.WriteLine(a[0]);")), "3");
}

// stdlib §4.4 (v0.350): "an extension member spelled `Sort` or `Sorted` is
// never consulted on an `Array<T>` receiver."
BEZEL_TEST(extension_vs_linq_7448, rule1_array_sort_instance_member_wins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Sort() end Sort; end;",
        " var a: Array<int> := [3,1,2]; ",
        "a.Sort(); Console.WriteLine(a[0]);")), "1");
}

// stdlib §4.4: `ToArray` is DECLARED on Array<T>, so it is the one §5-named
// operator the decline must not take on an Array receiver.
BEZEL_TEST(extension_vs_linq_7448, rule1_array_toarray_instance_member_wins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method ToArray(): owned Array<int> => [9]; end;",
        kArr, "var b := a.ToArray(); Console.WriteLine(b.Count);")), "3");
}

BEZEL_TEST(extension_vs_linq_7448, rule1_set_add_instance_member_wins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Set<int> public method Add(x: int) end Add; end;",
        " var s := new Set<int>(); ",
        "s.Add(1); s.Add(2); Console.WriteLine(s.Count);")), "2");
}

BEZEL_TEST(extension_vs_linq_7448,
           rule1_dictionary_containskey_instance_member_wins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Dictionary<string,int> public method ContainsKey(k: string): bool => false; end;",
        " var d := new Dictionary<string,int>(); d.Add(\"a\", 1); ",
        "Console.WriteLine(d.ContainsKey(\"a\"));")), "True");
}

// §26.5 rule 1 holds for a GENERIC extension head too (`extension Array<T>`).
BEZEL_TEST(extension_vs_linq_7448,
           rule1_generic_array_contains_instance_member_wins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<T> public method Contains(x: T): bool => false; end;",
        kArr, "Console.WriteLine(a.Contains(2));")), "True");
}

// CONTROL (stdlib §5): no user extension -> the stdlib operators run.
BEZEL_TEST(extension_vs_linq_7448, control_stdlib_sum_without_extension) {
    BEZEL_EXPECT_EQ(runAotOut(prog("", kArr,
        "Console.WriteLine(a.Sum()); Console.WriteLine(a.Max());")), "6\n3");
}

// CONTROL (§26.1): an extension on a user type still works.
BEZEL_TEST(extension_vs_linq_7448, control_extension_on_user_type) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module R; class Box public N: int := 7; end Box; "
        "extension Box public method Sum(): int => this.N + 70; end; "
        "class Program public static method Main(): int "
        " var b := new Box(); Console.WriteLine(b.Sum()); "
        " return 0; end Main; end Program;"), "77");
}

// Disposal path (receiver evaluated once, owned receiver freed): loop.
BEZEL_TEST(extension_vs_linq_7448, array_sum_extension_loop_100) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Sum(): int => this.Count; end;",
        " var t := 0; ",
        "for var i in 0..100 do var a: Array<int> := [1,2,3]; t := t + a.Sum(); end; "
        "Console.WriteLine(t);")), "303");
}
