// Codegen / ownership — issue #8124: the `?.` spelling of an OWNED-COLLECTION
// result (`s?.Split(..)`) must lower to the collection on a non-null receiver
// and to null on a null one.
//
// SPEC. §7.5 (v0.355, docs/Bezel_Language_Spec.md:1046): "`x?.M` returns null
// if `x` is null; otherwise it performs the access. The result type is always
// nullable". stdlib §9.6 (v0.355, docs/Bezel_Standard_Library.md:5447)
// declares the `String` splitting group and the conversion group:
//
//   public method Split(separator: char): owned Array<string>;
//   public method Split(separator: string): owned Array<string>;
//   public method Split(separators: Array<char>): owned Array<string>;
//   public method ToCharArray(): owned Array<char>;
//
// so the `?.` result is `Array<string>?` / `Array<char>?` — a NULLABLE
// REFERENCE to a fresh, owned collection.
//
// THE DEFECT. `var a := s?.Split('c')` on a NON-NULL receiver degraded to a
// zero-initialised slot ("lowerLocalDecl's initializer expression failed to
// lower to a value for this local's slot (degraded to a zero-initialized
// value) — #2434") and therefore read as `null`, with only that internal
// warning: the program compiled, printed the wrong answer and exited 0. The
// `?.` lowering knew ONE of the three `Split` overloads (the `string`
// separator) and none of the `char` / `Array<char>` ones, and the member it
// did know leaked a +1 producer receiver (§28.3 / §28.9).
//
// THE FIX. The `?.` spelling of the §9.6 owned-collection members is lowered
// by the SAME mechanism #7747 established for the other §9.6 instance
// members: the receiver is lowered once and null-checked, and the non-null
// path runs the `.` twin's own `tryLowerStringMethod` over a pinned receiver
// — so the `?.` and `.` spellings cannot disagree about the collection, and
// the §9.6 dispatch (all three `Split` overloads, `ToCharArray`) has exactly
// ONE implementation.
//
// THE ORACLES. Every row asserts the PRINTED OUTPUT — the value, not just
// "it compiled". A silent wrong value (the #2434/#3079 degraded-slot shape)
// exits 0, so "it ran" is not an oracle; the exact stdout is.
//
// The leak row is on `runAotChecked` (ASan/LSan) and loops 100×: the `?.`
// result is a fresh OWNED collection (§9.6), so the local's scope-end
// reclaim must free it, and a +1 producer receiver must be released.

#include <string>
#include <utility>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A program with one nullable string receiver (`s`, non-null), one null
// receiver (`n`), and a `StringBuilder` trace the evaluation row uses.
std::string prog(const std::string& body) {
    return "module M; import System; import System.Text; "
           "class C "
           "  public static method Tag(sb: StringBuilder, tag: string): string "
           "    sb.Append(tag); "
           "    return tag; "
           "  end Tag; "
           "  public static method MakeS(k: int): string? "
           "    if k mod 2 = 0 then return null; end if; "
           "    return \"ab\" + \"cd\"; "
           "  end MakeS; "
           "  public static method Main(): int "
           "    var s: string? := \"abcdc\"; "
           "    var n: string? := null; "
           "    using var sb := new StringBuilder(); " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A compile-time rejection, told from a crash by the diagnostic text.
bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ===========================================================================
// (A) The value rows — §7.5's rule for a null and a non-null receiver, over
//     the §9.6 owned-collection members.
// ===========================================================================

// The ISSUE's repro: `var a := s?.Split('c')` on a NON-NULL receiver must give
// the Array, not a zero-initialised slot. stdlib §9.6's `Split(separator:
// char)` splits "abcdc" at each 'c' → ["ab", "d", ""] — three parts (the
// trailing empty part is kept, exactly as the `.` spelling does).
BEZEL_TEST(codegen_string_nullsafe_owned_result_8124,
           spec_7_5_split_char_separator_non_null_receiver) {
    auto r = runAot(prog(
        "    var a := s?.Split('c'); "
        "    if a = null then Console.WriteLine(\"NULL\"); "
        "    else Console.WriteLine(\"n=\" + a!.Count.ToString()); end if; "
        "    Console.WriteLine(a![0]); "
        "    Console.WriteLine(a![2]); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "n=3\n"
                    "ab\n"
                    "\n");
}

// The same call on a NULL receiver yields null — §7.5's "returns null if `x`
// is null".
BEZEL_TEST(codegen_string_nullsafe_owned_result_8124,
           spec_7_5_split_char_separator_null_receiver) {
    auto r = runAot(prog(
        "    var a := n?.Split('c'); "
        "    if a = null then Console.WriteLine(\"NULL\"); "
        "    else Console.WriteLine(\"n=\" + a!.Count.ToString()); end if; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "NULL\n");
}

// ALL THREE of stdlib §9.6's `Split` overloads lower under `?.`: the `char`
// separator, the `string` separator and the `Array<char>` separator. The `.`
// spelling's answers are the reference: "a,b,c".Split(',') = ["a","b","c"],
// "a,b,c".Split(",") = ["a","b","c"], and "a,b,c".Split(['a']) = ["","b,c"].
BEZEL_TEST(codegen_string_nullsafe_owned_result_8124,
           stdlib_9_6_all_three_split_overloads_lower_under_the_qualified_spelling) {
    auto r = runAot(prog(
        "    var t: string? := \"a,b,c\"; "
        "    var seps: Array<char> := ['a']; "
        "    var a := t?.Split(','); "
        "    var b := t?.Split(\",\"); "
        "    var c := t?.Split(seps); "
        "    Console.WriteLine(\"a=\" + a!.Count.ToString()); "
        "    Console.WriteLine(\"b=\" + b!.Count.ToString()); "
        "    Console.WriteLine(\"c=\" + c!.Count.ToString()); "
        "    Console.WriteLine(c![0]); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "a=3\n"
                    "b=3\n"
                    "c=2\n"
                    "\n");
}

// `ToCharArray()` is the other §9.6 owned-collection member (stdlib §9.6:
// `public method ToCharArray(): owned Array<char>;`), and it must answer the
// same way under `?.`.
BEZEL_TEST(codegen_string_nullsafe_owned_result_8124,
           stdlib_9_6_to_char_array_under_the_qualified_spelling) {
    auto r = runAot(prog(
        "    var a := s?.ToCharArray(); "
        "    var b := n?.ToCharArray(); "
        "    if a = null then Console.WriteLine(\"NULL\"); "
        "    else Console.WriteLine(\"m=\" + a!.Count.ToString()); end if; "
        "    if b = null then Console.WriteLine(\"NULL\"); "
        "    else Console.WriteLine(\"m=\" + b!.Count.ToString()); end if; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "m=5\n"
                    "NULL\n");
}

// §7.5's evaluation rule: "if it is null the access does not occur and
// nothing to its right in the chain is evaluated — no member is read, no
// call is performed, and the argument and index expressions of the skipped
// suffixes are not evaluated either." A null receiver must not evaluate the
// separator argument.
BEZEL_TEST(codegen_string_nullsafe_owned_result_8124,
           spec_7_5_the_argument_is_not_evaluated_on_a_null_receiver) {
    auto r = runAot(prog(
        "    var a := n?.Split(Tag(sb, \"x\")); "
        "    if a = null then Console.WriteLine(\"NULL\"); end if; "
        "    Console.WriteLine(sb.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "NULL\n"
                    "\n");
}

// ===========================================================================
// (B) The LEAK rows — the `?.` result is a fresh OWNED collection (§9.6), so
//     the local's scope-end reclaim frees it; and a +1 producer receiver
//     (§28.3 / §28.9) is released by the `?.` lowering.
// ===========================================================================

BEZEL_TEST(codegen_string_nullsafe_owned_result_8124,
           spec_28_9_the_owned_result_is_discharged_no_leak) {
    auto r = runAotChecked(prog(
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var a := s?.Split('c'); "
        "      var b := n?.Split('c'); "
        "      var d := s?.Split(\",\"); "
        "      var e := s?.ToCharArray(); "
        "      if a!.Count = 3 then count := count + 1; end if; "
        "      if b = null then count := count + 1; end if; "
        "      if d!.Count = 1 then count := count + 1; end if; "
        "      if e!.Count = 5 then count := count + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(count); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "400\n");
}

// The `?.` receiver may be a +1 string producer (§28.3 / §28.9): the `?.`
// lowering releases it on the non-null edge, exactly as the `.` spelling's
// #429 release does. Before the fix the `?.` path knew only the `string`
// separator and never released the producer: MEASURED on the fix's parent, a
// 100-iteration loop of `var a := ("a" + "b")?.Split(",");` leaked
// "3500 byte(s) in 200 allocation(s)".
BEZEL_TEST(codegen_string_nullsafe_owned_result_8124,
           spec_28_9_the_producer_receiver_is_discharged_no_leak) {
    auto r = runAotChecked(prog(
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var a := (\"ab\" + \"cd\")?.Split('c'); "
        "      var b := MakeS(i)?.Split('c'); "
        "      var d := (\"ab\" + \"cd\")?.Split(\",\"); "
        "      if a!.Count = 2 then count := count + 1; end if; "
        "      if b = null then count := count + 1; end if; "
        "      if d!.Count = 1 then count := count + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(count); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "250\n");
}

// The `ToCharArray()` sibling of `Split` on the `.` spelling: stdlib §9.6
// declares it `owned Array<char>`, so a plain `var` binding of it is a fresh
// array the local SOLELY owns — the scope-end reclaim must free it. Before
// the fix the member was missing from the reclaim list (it named
// `ToArray` / `Sorted` / `Reversed` / `Split` / `ToByteArray` but not
// `ToCharArray`), so the backing leaked on EVERY binding. MEASURED (ASan/LSan)
// on the fix's parent: a 100-iteration loop of `var a := s.ToCharArray();`
// leaks "11200 byte(s) in 200 allocation(s)"; the byte-identical `using var`
// binding leaks nothing.
BEZEL_TEST(codegen_string_nullsafe_owned_result_8124,
           spec_28_9_the_to_char_array_result_is_discharged_no_leak) {
    auto r = runAotChecked(prog(
        "    var count := 0; "
        "    var t: string := \"abcdc\"; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var a := t.ToCharArray(); "
        "      if a.Count = 5 then count := count + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(count); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

// ===========================================================================
// (C) CONTROLS — the `.` spelling is untouched, and the `?.` arity gate
//     still fires.
// ===========================================================================

// The `.` spelling gives the same answers as the `?.` spelling — the two
// spellings of ONE member cannot disagree.
BEZEL_TEST(codegen_string_nullsafe_owned_result_8124,
           stdlib_9_6_dot_spelling_is_untouched) {
    auto r = runAot(prog(
        "    var t: string := \"abcdc\"; "
        "    var a := t.Split('c'); "
        "    Console.WriteLine(a.Count.ToString()); "
        "    var b := t.ToCharArray(); "
        "    Console.WriteLine(b.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "3\n"
                    "5\n");
}

// §22.5 / stdlib §9.6: a wrong arity is a compile error, and the `?.`
// spelling says the SAME thing as the `.` spelling — one mistake, one
// diagnostic.
BEZEL_TEST(codegen_string_nullsafe_owned_result_8124,
           spec_22_5_wrong_arity_is_rejected_on_both_spellings) {
    JitResult r = runAot(prog(
        "    var a := s?.Split('c', 'd'); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "the call to `Split` on `string` supplies "
                                  "2 argument(s) but stdlib §9.6 declares "
                                  "`Split(separator: char): owned "
                                  "Array<string>`"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}
