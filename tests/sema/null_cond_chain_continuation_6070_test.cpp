// Regression test for issue #6070 -- the §7.5 CHAIN CONTINUATION: a plain
// `.` suffix to the right of a `?.` segment (`s?.Length.ToString()`,
// `a?.Count.ToString()`) was REJECTED.
//
// THE DEFECT.
//
//   spec §7.5 (v0.357), *Evaluation*, verbatim:
//
//     "**The chain is the whole postfix sequence** (grammar A.7's
//     `PrimaryExpr`), its plain `.`, `[ ]` and `( )` suffixes included, and
//     a suffix to the right of a `?.` is typed at the value it meets on the
//     path where it runs — the preceding segment's own declared type, never
//     the nullable type the `?.` gives the chain's result: `s?.Length.ToString()`
//     over `s: string?` calls `ToString()` on an `int`, and the chain is a
//     `string?`. A segment whose own declared type is nullable needs its own
//     `?.` (or a `!`) before the next suffix, under §7.4 — `user?.Name?.Length`
//     where `Name` is a `string?` — and parentheses end the chain:
//     `(s?.Length).ToString()` is a member access on an `int?`, §7.4's error."
//
//   Before the fix, the compiler rejected the chain at the §7.4 catch-all
//   ("member access `.ToString` on a nullable expression result requires
//   narrowing to non-null first (§7.4)"), which is the pre-ruling reading of
//   the section. The owner's ruling (v0.355, quoted in the issue) is the
//   reading above: the chain continues, the suffix is typed at the SEGMENT's
//   own declared type, and the chain's result is nullable.
//
// THE ORACLE.
//
//   The VALUE rows assert the EXACT stdout of the AOT program (the real
//   value for a non-null receiver, `null` for a null one) — the strict
//   `runAot` path, not the deprecated in-process JIT harness.
//
//   The REJECTION rows assert the POSITIVE refusal: the program does NOT
//   run and the diagnostic names the §7.4 narrowing rule. They are the
//   shapes §7.5 does NOT admit as a continuation — a segment whose own
//   declared type is nullable, and a parenthesised segment.
//
//   The CONTROL rows assert that the `?.` access ALONE and the plain `.`
//   spelling were never affected -- they pass on BOTH arms, so a "refuse
//   everything" or "lower nothing" regression cannot pass.
//
//   No row asserts the ABSENCE of a diagnostic on a program that ran, so no
//   row is vacuous in the `check_vacuous_tests` sense (#3138).
//
// Cite: spec §7.5 (v0.357), §7.4 (v0.357); stdlib §9.6 (v0.357).

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"  // runAot / runAotOut
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "public class C "
           "  public static method Pick(present: bool): string? "
           "    if present then "
           "      return \"abc\"; "
           "    end; "
           "    return null; "
           "  end Pick; "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The §7.4 narrowing refusal — the diagnostic the chain continuation REPLACES
// for the admitted shapes, and KEEPS for the two §7.5 exclusions.
bool refusedAsNarrowing(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("requires narrowing") != std::string::npos &&
            d.message.find("(\xC2\xA7""7.4)") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// THE VALUE ROWS -- the chain continues over a non-null and a null receiver.
// This is the issue's own repro (`s?.Length.ToString()`), and the spec's own
// example (`s?.Length.ToString()` over `s: string?` calls `ToString()` on an
// `int`, and the chain is a `string?`).
// ---------------------------------------------------------------------------
BEZEL_TEST(null_cond_chain_continuation_6070,
           string_length_chain_continues_over_a_non_null_and_a_null_string_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := Pick(true); "
        "    var b := Pick(false); "
        "    var ra: string? := a?.Length.ToString(); "
        "    var rb: string? := b?.Length.ToString(); "
        "    Console.WriteLine(ra ?? \"null\"); "
        "    Console.WriteLine(rb ?? \"null\"); ")),
        "3\nnull");
}

// The COLLECTION form of the issue's repro: `a?.Count.ToString()` over an
// `Array<int>?`.
BEZEL_TEST(null_cond_chain_continuation_6070,
           array_count_chain_continues_over_a_non_null_and_a_null_array_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a: Array<int>? := [1, 2]; "
        "    var b: Array<int>? := null; "
        "    var ra: string? := a?.Count.ToString(); "
        "    var rb: string? := b?.Count.ToString(); "
        "    Console.WriteLine(ra ?? \"null\"); "
        "    Console.WriteLine(rb ?? \"null\"); ")),
        "2\nnull");
}

// The READ spelling of the continuation: `x?.Y.Z`, where `Z` is a PROPERTY
// read at the SEGMENT's own declared type (the owner's own MEASURED example
// on the issue — `e?.Key.Length` — has this shape). The segment is `Name`, a
// `string`, so `.Length` is read on a `string` — NOT on the `string?` the
// `?.` gives the chain's result.
BEZEL_TEST(null_cond_chain_continuation_6070,
           read_continuation_is_typed_at_the_segments_own_type_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "public class Box public Name: string := \"abc\"; end Box; "
        "public class C "
        "  public static method Main(): int "
        "    var a: Box? := new Box(); "
        "    var b: Box? := null; "
        "    var ra: int? := a?.Name.Length; "
        "    var rb: int? := b?.Name.Length; "
        "    Console.WriteLine(ra ?? -1); "
        "    Console.WriteLine(rb ?? -1); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "3\n-1");
}

// The chain's RESULT is nullable (§7.5): the `string?` annotation is
// admitted, and a NON-nullable one is refused by §7 (the nullability gate).
BEZEL_TEST(null_cond_chain_continuation_6070,
           chain_result_is_a_nullable_string_7_5) {
    auto r = runAot(prog(
        "    var a := Pick(true); "
        "    var r: string := a?.Length.ToString(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// The chain's result is USABLE as a nullable value: the `??` operand is the
// default when the segment is null.
BEZEL_TEST(null_cond_chain_continuation_6070,
           chain_result_coalesces_to_the_default_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := Pick(false); "
        "    var r: string? := a?.Length.ToString(); "
        "    Console.WriteLine(r ?? \"fallback\"); ")),
        "fallback");
}

// ---------------------------------------------------------------------------
// THE REJECTION ROWS -- the two shapes §7.5 does NOT admit as a chain
// continuation.
// ---------------------------------------------------------------------------

// §7.5: "A segment whose own declared type is nullable needs its own `?.`
// (or a `!`) before the next suffix, under §7.4." `Name` is declared
// `string?`, so `b?.Name.Length` is a member access on a nullable value.
BEZEL_TEST(null_cond_chain_continuation_6070,
           nullable_segment_needs_its_own_null_conditional_7_4_7_5) {
    auto r = runAot(
        "module M; import System; "
        "public class Box public Name: string? := null; end Box; "
        "public class C "
        "  public static method Main(): int "
        "    var b: Box? := new Box(); "
        "    var n := b?.Name.Length; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(refusedAsNarrowing(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §7.5: "parentheses end the chain: `(s?.Length).ToString()` is a member
// access on an `int?`, §7.4's error."
BEZEL_TEST(null_cond_chain_continuation_6070,
           parenthesised_segment_ends_the_chain_7_4_7_5) {
    auto r = runAot(prog(
        "    var a := Pick(true); "
        "    var r: string? := (a?.Length).ToString(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(refusedAsNarrowing(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// THE CONTROL ROWS -- the `?.` access ALONE and the plain `.` spelling are
// untouched (they pass on BOTH arms).
// ---------------------------------------------------------------------------
BEZEL_TEST(null_cond_chain_continuation_6070,
           null_cond_access_alone_is_unchanged_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := Pick(true); "
        "    var n: int? := a?.Length; "
        "    Console.WriteLine(n ?? -1); ")),
        "3");
}

BEZEL_TEST(null_cond_chain_continuation_6070,
           plain_access_spelling_is_unchanged_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := Pick(true); "
        "    var n: int := a!.Length; "
        "    Console.WriteLine(n); ")),
        "3");
}
