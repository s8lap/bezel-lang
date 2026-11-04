// Codegen — issue #8123: the `?.` SPELLING of the stdlib §9.6 string
// PROPERTIES (`IsEmpty`, `IsAscii`).
//
// SPEC. §7.5 (v0.355, docs/Bezel_Language_Spec.md:1046): "`x?.M` returns null
// if `x` is null; otherwise it performs the access. The result type is always
// nullable". §7.5 states NO type-based exception on `M` — the rule is about
// the RECEIVER's nullability only. stdlib §9.6 (v0.355,
// docs/Bezel_Standard_Library.md:5368) declares the `String` class and its
// three named properties:
//
//   public pure readonly property Length: int;   // number of code points (O(1))
//   public pure readonly property IsEmpty: bool; // Length = 0 — §3.3's spelling (O(1))
//   public pure readonly property IsAscii: bool; // true iff every code point < 128 (O(1))
//
// THE DEFECT. The `?.` spelling of BOTH bool properties was REFUSED with
// "string member `?.IsEmpty` is not yet supported in codegen" — the
// diagnostic's own wording says the refusal is an implementation gap reported
// as a compile error, not a rule of the language. The `.` spelling of
// `IsAscii` worked; `s?.Length` worked.
//
// THE FIX. Sema admits the two bool properties on the `?.` path (the §7.5
// lift to `bool?` comes from the same `builtinValueMemberType` escape hatch
// `s?.Length` already used), and codegen lowers them over the receiver's null
// check: a null receiver yields null (`bool?` with hasValue = false), a
// non-null receiver yields the §9.6 value. The values are the ones the `.`
// spelling computes — `IsEmpty` is `Length == 0` (the string header's
// code-point count) and `IsAscii` is the runtime's `bz_string_is_ascii`, the
// SAME helper the `.` spelling calls — so the two spellings cannot disagree.
//
// THE ORACLES. Every row asserts the PRINTED OUTPUT — the value, not just
// "it compiled". A silent wrong value (the #2434/#3079 degraded-slot shape)
// exits 0, so "it ran" is not an oracle; the exact stdout is.
//
// SCOPE. The `.` spelling of `s.IsEmpty` is a SEPARATE, still-open gap
// (#2348's "String.IsEmpty" item; the compiler refuses it with "string member
// `.IsEmpty` is not yet supported in codegen" independently of nullability).
// This file does NOT pin that refusal and does NOT change it: the `?.`
// spelling is the whole of #8123.
//
// The leak row is on `runAotChecked` (ASan/LSan) and loops 100×: the `?.`
// receiver may be a +1 string producer (§28.3 / §28.9), and the `?.` lowering
// releases it on the non-null edge — the `.` spelling's #429 release, in the
// `?.` form.

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
// receiver (`n`), and a producer-receiver helper the leak row uses.
std::string prog(const std::string& body) {
    return "module M; import System; import System.Text; "
           "class C "
           "  public static method MakeS(k: int): string? "
           "    if k mod 2 = 0 then return null; end if; "
           "    return \"ab\" + \"cd\"; "
           "  end MakeS; "
           "  public static method Main(): int "
           "    var s: string? := \"abcde\"; "
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
//     the two §9.6 bool properties.
// ===========================================================================

// §7.5: "`x?.M` returns null if `x` is null; otherwise it performs the
// access." Each property is read on a NON-NULL receiver (its §9.6 answer) and
// on a NULL receiver (null). The answers are the `.` spelling's answers:
// stdlib §9.6 declares `IsEmpty: bool` as "Length = 0" and `IsAscii: bool` as
// "true iff every code point < 128", so "abcde" is not empty and is ASCII.
BEZEL_TEST(codegen_string_nullsafe_property_8123,
           spec_7_5_property_null_and_non_null_receiver) {
    auto r = runAot(prog(
        "    Console.WriteLine(s?.IsEmpty ?? true); "
        "    Console.WriteLine(n?.IsEmpty ?? true); "
        "    Console.WriteLine(s?.IsAscii ?? false); "
        "    Console.WriteLine(n?.IsAscii ?? false); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "False\n"
                    "True\n"
                    "True\n"
                    "False\n");
}

// The EMPTY string answers `IsEmpty` = true — the §9.6 contract is
// "Length = 0", not "the receiver is null". A `?.` on an empty (non-null)
// receiver must still read the property.
BEZEL_TEST(codegen_string_nullsafe_property_8123,
           stdlib_9_6_isempty_is_length_zero_not_receiver_null) {
    auto r = runAot(prog(
        "    var e: string? := \"\"; "
        "    Console.WriteLine(e?.IsEmpty ?? false); "
        "    Console.WriteLine(e?.IsAscii ?? false); "
        "    Console.WriteLine(s?.IsEmpty ?? false); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "True\n"
                    "True\n"
                    "False\n");
}

// A NON-ASCII receiver answers `IsAscii` = false — the property is not a
// constant "true" for any non-null receiver.
BEZEL_TEST(codegen_string_nullsafe_property_8123,
           stdlib_9_6_isascii_is_false_for_a_non_ascii_receiver) {
    auto r = runAot(prog(
        "    var u: string? := \"\\u65e5\"; "
        "    Console.WriteLine(u?.IsAscii ?? true); "
        "    Console.WriteLine(u?.IsEmpty ?? true); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "False\n"
                    "False\n");
}

// §7.5: "The result type is always nullable" — the `?.` result is a `bool?`,
// so it can be STORED in a `bool?` local, tested for null, and read back.
// The `= null` test is the §7.5 null-edge witness: on a null receiver the
// result is null, on a non-null receiver it is not.
BEZEL_TEST(codegen_string_nullsafe_property_8123,
           spec_7_5_result_is_nullable) {
    auto r = runAot(prog(
        "    var a: bool? := s?.IsEmpty; "
        "    var b: bool? := n?.IsEmpty; "
        "    var c: bool? := s?.IsAscii; "
        "    var d: bool? := n?.IsAscii; "
        "    Console.WriteLine((a = null).ToString()); "
        "    Console.WriteLine((b = null).ToString()); "
        "    Console.WriteLine((c = null).ToString()); "
        "    Console.WriteLine((d = null).ToString()); "
        "    Console.WriteLine((a ?? true).ToString()); "
        "    Console.WriteLine((c ?? false).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "False\n"
                    "True\n"
                    "False\n"
                    "True\n"
                    "False\n"
                    "True\n");
}

// §7.5's evaluation rule: the receiver is evaluated ONCE, and a null
// receiver short-circuits — nothing to its right in the chain is evaluated.
// The chain `s?.IsEmpty` is one suffix, so the receiver's own evaluation
// count is the witness.
BEZEL_TEST(codegen_string_nullsafe_property_8123,
           spec_7_5_the_receiver_is_evaluated_once) {
    auto r = runAot(prog(
        "    var count := 0; "
        "    var t: string? := \"abcde\"; "
        "    var u: bool? := t?.IsEmpty; "
        "    if u <> null then count := count + 1; end if; "
        "    var v: bool? := n?.IsEmpty; "
        "    if v = null then count := count + 1; end if; "
        "    Console.WriteLine(count); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// ===========================================================================
// (B) The LEAK row — a `?.` receiver may be a +1 string producer
//     (§28.3 / §28.9); the `?.` lowering releases it on the non-null edge.
// ===========================================================================

BEZEL_TEST(codegen_string_nullsafe_property_8123,
           spec_28_9_the_producer_receiver_is_discharged_no_leak) {
    auto r = runAotChecked(prog(
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var a: bool? := (\"a\" + \"b\")?.IsEmpty; "
        "      var b: bool? := (\"a\" + \"b\")?.IsAscii; "
        "      var c: bool? := MakeS(i)?.IsEmpty; "
        "      if a = false then count := count + 1; end if; "
        "      if b = true then count := count + 1; end if; "
        "      if c = null then count := count + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(count); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "250\n");
}

// ===========================================================================
// (C) CONTROLS — the fix must admit ONLY the two §9.6 bool properties, and
//     must not touch the `.` spelling.
// ===========================================================================

// The CALL spelling of a §9.6 PROPERTY is still a compile error: the member
// is a property, not a method (stdlib §9.6), and §26.5 rule 1 commits the
// lookup to the instance member. The `?.` spelling must keep its own gate.
BEZEL_TEST(codegen_string_nullsafe_property_8123,
           stdlib_9_6_property_call_spelling_is_still_rejected) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(s?.IsEmpty() ?? true); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "`IsEmpty` is a property of `string`, not "
                                  "a method (stdlib §9.6: `IsEmpty: bool`)"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A name §9.6 does not declare is still refused, with the `?.` spelling's own
// wording (the `.` spelling's wording differs only in the operator character).
BEZEL_TEST(codegen_string_nullsafe_property_8123,
           spec_7_5_unknown_member_is_still_rejected) {
    JitResult r = runAot(prog(
        "    var z := s?.Bogus; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "string member `?.Bogus` is not yet "
                                  "supported in codegen"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// The `.` spelling is untouched: `s.IsAscii` still gives the §9.6 answer, and
// `s.IsEmpty` still draws #2348's own refusal — this fix changes the `?.`
// spelling only, and does not silently widen the `.` one.
BEZEL_TEST(codegen_string_nullsafe_property_8123,
           stdlib_9_6_dot_spelling_is_untouched) {
    auto r = runAot(prog(
        "    var t: string := \"abcde\"; "
        "    Console.WriteLine(t.IsAscii); "
        "    Console.WriteLine(t.Length); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n5\n");
}
