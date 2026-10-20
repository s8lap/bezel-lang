// #6542 (spec §17.5, §5.1, v0.338; stdlib §9.6): `in` over a STRING receiver
// rejected a non-`char` left operand at the CODEGEN layer, blaming the LLVM
// lowering arm instead of the §5.1 conversion site.
//
// MEASURED on the pre-fix base (`178d49d1`), every one of these compiles
// through sema CLEANLY and dies in `LlvmBackend_Exprs.cpp` with
//
//     not yet implemented: codegen for this `in` expression form
//
//   `1 in "abc"`            (int)
//   `1.5 in "abc"`          (double)
//   `true in "abc"`         (bool)
//   `b: byte` in "abc"      (byte)
//
// which points the reader at the codegen dispatch — the message shape that
// invites "just implement the arm" as a follow-up. But there is NO codegen
// hole to close: `String` already declares BOTH `Contains` overloads and
// codegen already lowers both (`LlvmBackend_Exprs.cpp`'s string-receiver arm
// tests `lTy->primitive == String` then `== Char`). §17.5 says `x in c` IS
// the call `c.Contains(x)`, so the left operand is admitted exactly when that
// call's argument is admitted — and §5.1's table lists `char` as a SOURCE
// only, while §4.2's integer-types table omits `char` entirely, so §5.1's
// constant narrowing has no `char` target either (§5.2 makes integer→`char`
// the EXPLICIT `char(n)` conversion, with its code-point rule). All four
// programs are CORRECTLY REJECTED; only the LAYER was wrong.
//
// The fix gives the string receiver its own case in sema's `in` block: a
// receiver whose type is `string` routes its left operand through the same
// `checkAssignable` the `Contains` call form uses, against `char` — the
// `string` overload is admitted by its own identity (a `string` left operand
// is §9.6's substring test, and is not routed through the `char` check).
//
// THE CONTROL SET (Category B) is the point of this file. Each row below
// compiles and RUNS on both sides of the fix and pins the CORRECT values, so
// a "fix" that implements a codegen arm for `int`/`double` in a string —
// which would make these REJECT-programs COMPILE — fails this file rather
// than passing it. That is the failure mode the issue warns about, and it is
// why the wording assertions alone would not be enough.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

bool hasErrorContaining(const JitResult& r, const char* needle) {
    return hasErrorContaining(r, std::string(needle));
}

// The §5.1 conversion-site needle, quoted verbatim. Also asserted ABSENT so a
// diagnostic that merely mentions §5.1 while still blaming codegen cannot
// satisfy this file.
const char* kConversionSite =
    "left operand of `in` over a `string` receiver has a type that is not "
    "implicitly convertible to the target type (\xC2\xA7""5.1)";
const char* kWrongLayer = "not yet implemented: codegen for this `in` expression";

// Wraps one `in`-over-string expression in a runnable Main.
std::string prog(const std::string& expr) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    Console.WriteLine((" + expr + ").ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ===========================================================================
// Category A — the rejected spellings now name the CONVERSION SITE
// ===========================================================================

// The issue's own repro #1: a `double` left operand. Pre-fix: "not yet
// implemented: codegen for this `in` expression form" (LlvmBackend_Exprs.cpp).
// Post-fix: §5.1, at sema. §5.1 admits no `double`→`char` row.
BEZEL_TEST(in_over_string_left_operand_6542,
           double_left_operand_rejected_at_conversion_site_spec_5_1) {
    auto r = runAot(prog("1.5 in \"abc\""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kConversionSite));
    // The layer the defect was about: this needle must be GONE. Asserted as
    // an absence on a program that does NOT compile, so it is not the
    // "assert no warning on a compiling program" trap — runAot does fill
    // diagnostics on a reject.
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kWrongLayer));
}

// The issue's own repro #2: an `int` left operand.
BEZEL_TEST(in_over_string_left_operand_6542,
           int_left_operand_rejected_at_conversion_site_spec_5_1) {
    auto r = runAot(prog("1 in \"abc\""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kConversionSite));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kWrongLayer));
}

// An `int` CONSTANT. This is the row that would flip if §5.1's constant
// narrowing were (wrongly) taken to reach `char`: §4.2's integer-types table
// does not list `char`, and §5.2 makes integer→`char` explicit. Pre-fix this
// reached codegen like the run-time `int` did, so the narrowing question was
// never actually reached — the fix is what puts it on the §5.1 path.
BEZEL_TEST(in_over_string_left_operand_6542,
           int_constant_left_operand_rejected_spec_5_1_no_char_target) {
    auto r = runAot(prog("99 in \"abc\""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kConversionSite));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kWrongLayer));
}

// `bool`. §5.1: "Between integers and `bool`" is never implicit, and §5.2
// adds that a `bool` source "does not convert to … `char` (a code point, not
// a number)". Pre-fix: the codegen NYI.
BEZEL_TEST(in_over_string_left_operand_6542,
           bool_left_operand_rejected_at_conversion_site_spec_5_1) {
    auto r = runAot(prog("true in \"abc\""));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kConversionSite));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kWrongLayer));
}

// `byte`. A WIDENING integer — the row nearest the §5.1 table, and the one a
// too-broad predicate would let through by accident if it read "any integer
// is fine". §5.1's `byte` row lists short/ushort/int/uint/long/ulong/float/
// double/decimal; `char` is not on it.
BEZEL_TEST(in_over_string_left_operand_6542,
           byte_left_operand_rejected_at_conversion_site_spec_5_1) {
    auto r = runAot(R"(
module M;
class C
  public static method Main(): int
    var b: byte := 98;
    Console.WriteLine((b in "abc").ToString());
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kConversionSite));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kWrongLayer));
}

// The §7 nullable face: a `char?` left operand. §17.5's nullable paragraph
// and §5.1 both say `T?` does not implicitly convert to `T`. Pre-fix this
// reached codegen and died in IR VERIFICATION ("Both operands to ICmp
// instruction are not of the same type!") — the same wrong-layer attribution
// one layer deeper, and a much louder failure. The needle is checkAssignable's
// OWN §7 wording, so it is not the §5.1 string; it is pinned separately so a
// regression that merged the two messages would be visible.
BEZEL_TEST(in_over_string_left_operand_6542,
           nullable_char_left_operand_rejected_at_conversion_site_spec_7) {
    auto r = runAot(R"(
module M;
class C
  public static method Main(): int
    var c: char? := 'c';
    Console.WriteLine((c in "abc").ToString());
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "left operand of `in` over a `string` receiver is nullable but "
           "the target type is non-nullable (\xC2\xA7""7)"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kWrongLayer));
}

// ===========================================================================
// Category B — the CONTROL SET: every legal `char`/`string` spelling still
// lowers and produces the right answer. A "fix" that implemented a codegen arm
// instead of moving the rejection would make Category A COMPILE and these rows
// unchanged — this half is what fails that attempt.
// ===========================================================================

// `'c' in "abc"` -> True; `'z' in "abc"` -> False. The spec's own §17.5
// example (`var hasChar := 'a' in "abcdef";`) is the same shape.
BEZEL_TEST(in_over_string_left_operand_6542,
           char_literal_left_operand_runs_spec_17_5) {
    auto r = runAot(R"(
module M;
class C
  public static method Main(): int
    Console.WriteLine(('c' in "abc").ToString());
    Console.WriteLine(('z' in "abc").ToString());
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// A `char` VARIABLE, read at run time — the row that reads the data the fix
// handles, so a predicate keyed on the literal spelling rather than the type
// cannot pass.
BEZEL_TEST(in_over_string_left_operand_6542,
           char_variable_left_operand_runs_spec_17_5) {
    auto r = runAot(R"(
module M;
class C
  public static method Main(): int
    var c: char := 'b';
    Console.WriteLine((c in "abc").ToString());
    c := 'q';
    Console.WriteLine((c in "abc").ToString());
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// An `if`-expression left operand: the conditional's join type is `char`, so
// the fix's check must read the JOINED type, not either branch's spelling.
BEZEL_TEST(in_over_string_left_operand_6542,
           if_expression_left_operand_runs_spec_17_5) {
    auto r = runAot(R"(
module M;
class C
  public static method Main(): int
    var pick: bool := false;
    Console.WriteLine(((if pick then 'b' else 'z') in "abc").ToString());
    pick := true;
    Console.WriteLine(((if pick then 'b' else 'z') in "abc").ToString());
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\nTrue\n");
}

// A `string`-TYPED RECEIVER (a variable, not a literal): the receiver's
// spelling must not change the verdict.
BEZEL_TEST(in_over_string_left_operand_6542,
           string_variable_receiver_runs_spec_17_5) {
    auto r = runAot(R"(
module M;
class C
  public static method Main(): int
    var s: string := "xabcx";
    Console.WriteLine(('a' in s).ToString());
    Console.WriteLine(('q' in s).ToString());
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// The `string in string` OVERLOAD — §9.6's substring test. This is the row a
// fix that routed EVERY string-receiver left operand through `checkAssignable`
// against `char` would REJECT: `string` does not convert to `char` in §5.1.
// Unchanged on both sides of the fix, and it is what keeps the fix's
// enumeration at "char OR string", not "char".
BEZEL_TEST(in_over_string_left_operand_6542,
           string_left_operand_substring_runs_spec_9_6) {
    auto r = runAot(R"(
module M;
class C
  public static method Main(): int
    Console.WriteLine(("bc" in "abcd").ToString());
    Console.WriteLine(("zz" in "abcd").ToString());
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// A `string` ALIAS receiver — the alias resolves to the `string` primitive, so
// the receiver-side predicate must read the resolved type. MEASURED pre-fix:
// this program compiled and ran; a predicate keyed on the head SPELLING
// (`Name`, not `string`) would have silently skipped it and left `k in n`
// blamed on codegen.
BEZEL_TEST(in_over_string_left_operand_6542,
           string_alias_receiver_still_checks_left_operand_spec_5_1) {
    auto r = runAot(R"(
module M;
type Name := string;
class C
  public static method Main(): int
    var n: Name := "abc";
    Console.WriteLine(('b' in n).ToString());
    var k: int := 1;
    Console.WriteLine((k in n).ToString());
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kConversionSite));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kWrongLayer));
}

// A `char?` made non-null by `??` (§7.7) — one of §17.5's two named remedies,
// and the spelling that lowers correctly today, so it must keep working.
//
// The OTHER §17.5-named remedy, a §7.4 `if c is null` guard, is NOT pinned
// here and MEASURED does not lower: with the narrowed `c: char?` it dies in IR
// VERIFICATION ("Both operands to ICmp instruction are not of the same type!")
// — on the PRE-FIX base as well as after this fix, because the left operand
// keeps its tagged `{i8,i32}` slot and the string-receiver arm `CreateICmpEQ`s
// it against a bare code point. That is a LOWERING defect in the #2625
// family, not the layer-attribution defect #6542 is about, and it is filed
// separately as #6547 rather than folded in here: fixing it means touching
// the `in` lowering arm, which is precisely the "implement the arm instead
// of moving the rejection" move this file's controls exist to catch. Note the
// fix does not make it WORSE — the narrowed `char?` is still admitted by
// sema and still fails at the same place, because `fromExpr` is threaded into
// the check so §7.4's narrowing fact is honoured.
BEZEL_TEST(in_over_string_left_operand_6542,
           coalesced_nullable_char_left_operand_still_runs_spec_7_7) {
    auto r = runAot(R"(
module M;
class C
  public static method Main(): int
    var c: char? := 'c';
    Console.WriteLine(((c ?? 'z') in "abc").ToString());
    c := null;
    Console.WriteLine(((c ?? 'z') in "abc").ToString());
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}