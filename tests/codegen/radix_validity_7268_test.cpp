// Tests for #7268 — the radix-VALIDITY rule of stdlib §9.1 (v0.345).
//
// WHAT THE RULE IS.
//
// stdlib §9.1 declares, for every integer type:
//
//   public static pure method Parse(s: string, radix: int): int;
//     // radix in {2, 8, 10, 16}; any other radix raises ArgumentException —
//     //   a caller bug, not input failure
//   public static method TryParse(s: string, radix: int, out result: int): bool;
//     // … an invalid radix raises ArgumentException from both forms
//   public pure method ToString(radix: int): string;
//
// and its "radix alphabet" paragraph says of `ToString(radix)`: "emits the
// **lower-case** digits … and takes the same radix set, raising
// `ArgumentException` for any other". stdlib §2.3 carries the same rule for
// `Convert.ToString(value: int, toBase: int): string` — "bases 2, 8, 10, 16;
// ArgumentException otherwise" — and §2.3 says that form "returns the text
// `value.ToString(toBase)` returns", so the two sites answer alike.
//
// THE ADMITTED SET IS {2, 8, 10, 16} — FOUR values.
//
// WHAT WAS WRONG (measured on the base arm, this branch's parent).
//
// There was NO radix-validity check at ANY of the four sites, and the one
// root cause had THREE symptoms:
//
//   1. a CRASH. `int.Parse("ff", 'a')`, `int.TryParse("ff", 'a', out v)`,
//      `int.Parse("ff", 97)`, `int.Parse("11", 37)`, `int.Parse("11", 1)`,
//      `int.Parse("11", -1)` and `int.Parse("11", 2147483647)` all COMPILED
//      and then died with SIGSEGV (exit 139). The mechanism: the lowering
//      calls `strtoll(data, &endptr, radix)`, and the C library leaves
//      `endptr` UNWRITTEN for a base outside 2..36; the lowering then
//      dereferences that uninitialized pointer. The boundary is exact —
//      radix 36 parsed, radix 37 crashed.
//   2. a SILENT DECIMAL. `(42).ToString(3)` and `Convert.ToString(42, 3)`
//      printed "42" — the decimal text — instead of raising.
//   3. a SILENT WRONG PARSE. `int.Parse("11", 36)` printed "37" and
//      `int.Parse("11", 3)` printed "4": the C library accepts any base in
//      2..36, so the radix was honoured even though §9.1 admits only four.
//
// The `char`-radix ARGUMENT half of this shape is the RESIDUAL of #7285
// (`int.Parse("ff", 'a')`): §5.1's widening table admits `char` → `int`, so
// the argument BINDS correctly and the crash is a VALUE defect, not a
// type-binding one. It is the same rule, so it is fixed here.
//
// THE RULING: THE CHECK IS A RUNTIME CHECK, FOR A CONSTANT RADIX TOO.
//
// §9.1's own words are "raising `ArgumentException`", and an
// `ArgumentException` is a RUNTIME exception. The conformance corpus agrees
// and is decisive: `l9_1_0006` (stdlib §9.1, `int.Parse("10", 7)`) and
// `l9_1_0016` (stdlib §9.1, `int.TryParse("10", 3, out var v)`) and
// `l2_3_0003` (stdlib §2.3, `Convert.ToString(42, 3)`) are all `expect: run`
// cases that CATCH the exception at RUN TIME, with a LITERAL radix. A
// compile-time rejection of a constant radix would make those three
// programs fail to compile, and would contradict §9.1's "raises" outright.
//
// So the constant radix raises at RUN TIME, exactly like the variable one.
//
// THE ORACLES — TWO SHAPES, ON PURPOSE.
//
//   * The CAUGHT shape (`expectRaisedArgumentException`): the program catches
//     `ArgumentException` and returns 1. The catch arm is TYPED, so a return
//     code of 1 also pins the exception TYPE.
//   * The UNCAUGHT shape (`expectUncaughtArgumentException`): the program
//     calls the member directly and the exception reaches the no-handler
//     boundary — the program RAN, and exited 70 (EX_SOFTWARE), the house's
//     uncaught-exception code (spec §33.3).
//
// The UNCAUGHT shape is the MEMORY-SAFETY shape, and the `r.ran` assertion
// in it is LOAD-BEARING: on the pre-fix arm those programs SEGFAULTED, so
// `ran` is false and the row fails with "assertion failed: r.ran" — the
// CRASH, not a value mismatch. (The CAUGHT shape is not a memory-safety
// shape: the `try` changes the stack frame, and the uninitialized
// end-pointer slot then reads as 0 instead of a wild pointer — the program
// runs and answers wrongly. That is why the crash rows use the UNCAUGHT
// shape.)

#include <string>

#include "support/aot_support.hpp"   // runAot — the AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program CAUGHT an `ArgumentException`: it ran, and the TYPED catch arm
// returned 1.
void expectRaisedArgumentException(const JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// The program raised an UNCAUGHT `ArgumentException`: it RAN (so it did not
// SEGFAULT) and the runtime reported it at the no-handler boundary — exit
// 70, the house's uncaught-exception code (spec §33.3).
//
// The `r.ran` assertion comes FIRST and is LOAD-BEARING: on the pre-fix arm
// these programs SEGFAULTED, and this assertion is what makes that visible
// as "the program did not run" rather than as a wrong value.
void expectUncaughtArgumentException(const JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 70);
}

// A `Parse(s, radix)` program whose body is `EXPR` — the radix argument is
// written in the call itself, so the SHAPE of the radix (a literal, a `uint`
// literal, a `char` literal, a folded constant expression, a runtime local)
// is the caller's choice.
//
// `catch_` selects the CAUGHT shape (the exception is handled and the
// program returns 1); otherwise the exception is left uncaught.
std::string parseProg(const std::string& expr, bool catch_ = true) {
    if (!catch_)
        return "module M;\n"
               "public class P\n"
               "  public static method Main(): int\n"
               "    Console.WriteLine(int.Parse(\"ff\", " + expr + "));\n"
               "    return 0;\n"
               "  end Main;\n"
               "end P;\n";
    return "module M;\n"
           "public class P\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      _ := int.Parse(\"ff\", " + expr + ");\n"
           "      return 0;\n"
           "    catch\n"
           "      ArgumentException => return 1;\n"
           "    end;\n"
           "  end Main;\n"
           "end P;\n";
}

// A `TryParse(s, radix, out result)` program.
std::string tryParseProg(const std::string& expr, bool catch_ = true) {
    if (!catch_)
        return "module M;\n"
               "public class P\n"
               "  public static method Main(): int\n"
               "    var v: int := 0;\n"
               "    Console.WriteLine(int.TryParse(\"ff\", " + expr + ", out v));\n"
               "    return 0;\n"
               "  end Main;\n"
               "end P;\n";
    return "module M;\n"
           "public class P\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      var v: int := 0;\n"
           "      _ := int.TryParse(\"ff\", " + expr + ", out v);\n"
           "      return 0;\n"
           "    catch\n"
           "      ArgumentException => return 1;\n"
           "    end;\n"
           "  end Main;\n"
           "end P;\n";
}

// A `ToString(radix)` program on the primitive receiver.
std::string toStringProg(const std::string& recv, const std::string& expr,
                          bool catch_ = true) {
    if (!catch_)
        return "module M;\n"
               "public class P\n"
               "  public static method Main(): int\n"
               "    Console.WriteLine((" + recv + ").ToString(" + expr + "));\n"
               "    return 0;\n"
               "  end Main;\n"
               "end P;\n";
    return "module M;\n"
           "public class P\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      _ := (" + recv + ").ToString(" + expr + ");\n"
           "      return 0;\n"
           "    catch\n"
           "      ArgumentException => return 1;\n"
           "    end;\n"
           "  end Main;\n"
           "end P;\n";
}

// A `Convert.ToString(value, toBase)` program.
std::string convertProg(const std::string& expr, bool catch_ = true) {
    if (!catch_)
        return "module M;\n"
               "public class P\n"
               "  public static method Main(): int\n"
               "    Console.WriteLine(Convert.ToString(42, " + expr + "));\n"
               "    return 0;\n"
               "  end Main;\n"
               "end P;\n";
    return "module M;\n"
           "public class P\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      _ := Convert.ToString(42, " + expr + ");\n"
           "      return 0;\n"
           "    catch\n"
           "      ArgumentException => return 1;\n"
           "    end;\n"
           "  end Main;\n"
           "end P;\n";
}

// A program whose radix is a RUNTIME value the compiler cannot fold.
std::string runtimeRadixProg(const std::string& body, int radix) {
    return "module M;\n"
           "public class P\n"
           "  public static method R(): int\n"
           "    return " + std::to_string(radix) + ";\n"
           "  end R;\n"
           "  public static method Main(): int\n"
           "    try\n" + body +
           "      return 0;\n"
           "    catch\n"
           "      ArgumentException => return 1;\n"
           "    end;\n"
           "  end Main;\n"
           "end P;\n";
}

}  // namespace

// ─── THE MEMORY-SAFETY ROWS (the UNCAUGHT shape) ──────────────────────────
//
// On the pre-fix arm each of these programs SEGFAULTED (exit 139): the
// `strtoll` end-pointer was left UNWRITTEN for a base outside 2..36 and the
// lowering dereferenced it. `ran` is false there, so the row fails with
// "assertion failed: r.ran" — the CRASH, not a wrong value.

// The `char` radix (the #7285 residual): §5.1's widening table admits
// `char` → `int`, so the argument binds and the VALUE 97 reaches the parser.
BEZEL_TEST(radix_validity_7268, char_radix_at_parse_raises_instead_of_crashing) {
    // stdlib §9.1: "radix in {2, 8, 10, 16}; any other radix raises
    // ArgumentException". The char 'a' is 97, outside the set.
    expectUncaughtArgumentException(runAot(parseProg("'a'", /*catch_=*/false)));
}

// The same `char` radix at the `TryParse` site.
BEZEL_TEST(radix_validity_7268, char_radix_at_tryparse_raises_instead_of_crashing) {
    // stdlib §9.1: "an invalid radix raises ArgumentException from both
    // forms" — the `TryParse` form included.
    expectUncaughtArgumentException(
        runAot(tryParseProg("'a'", /*catch_=*/false)));
}

// An `int` radix past the 36-symbol alphabet — the coordinator's measured
// `int.Parse("ff", 97)`.
BEZEL_TEST(radix_validity_7268, radix_97_at_parse_raises_instead_of_crashing) {
    expectUncaughtArgumentException(runAot(parseProg("97", /*catch_=*/false)));
}

// The EXACT boundary: radix 36 is inside the C library's alphabet (so the
// pre-fix compiler PARSED it, silently, in base 36) and radix 37 is one past
// it (so the pre-fix compiler CRASHED). Both are outside §9.1's admitted set,
// so both must raise.
BEZEL_TEST(radix_validity_7268, radix_37_at_parse_raises_instead_of_crashing) {
    expectUncaughtArgumentException(runAot(parseProg("37", /*catch_=*/false)));
}

// The low boundary: radix 1 is below the C library's alphabet.
BEZEL_TEST(radix_validity_7268, radix_1_at_parse_raises_instead_of_crashing) {
    expectUncaughtArgumentException(runAot(parseProg("1", /*catch_=*/false)));
}

// A negative radix.
BEZEL_TEST(radix_validity_7268, negative_radix_at_parse_raises_instead_of_crashing) {
    expectUncaughtArgumentException(runAot(parseProg("-1", /*catch_=*/false)));
}

// The largest `int` — the far end of the space.
BEZEL_TEST(radix_validity_7268, radix_int_max_at_parse_raises_instead_of_crashing) {
    expectUncaughtArgumentException(
        runAot(parseProg("2147483647", /*catch_=*/false)));
}

// The same crash at the `TryParse` site.
BEZEL_TEST(radix_validity_7268, radix_37_at_tryparse_raises_instead_of_crashing) {
    expectUncaughtArgumentException(
        runAot(tryParseProg("37", /*catch_=*/false)));
}

// The `TryParse` site's own low boundary.
BEZEL_TEST(radix_validity_7268, radix_1_at_tryparse_raises_instead_of_crashing) {
    expectUncaughtArgumentException(
        runAot(tryParseProg("1", /*catch_=*/false)));
}

// ─── THE EXCEPTION-TYPE ROWS (the CAUGHT shape) ───────────────────────────
//
// The catch arm is TYPED, so these rows pin the exception TYPE as well as
// the fact that the exception is raised at all.

// The `char` radix at the Parse site — the #7285 residual, typed.
BEZEL_TEST(radix_validity_7268, char_radix_at_parse_raises_argument_exception) {
    expectRaisedArgumentException(runAot(parseProg("'a'")));
}

// The `char` radix at the TryParse site — the #7285 residual, typed.
BEZEL_TEST(radix_validity_7268, char_radix_at_tryparse_raises_argument_exception) {
    expectRaisedArgumentException(runAot(tryParseProg("'a'")));
}

// The Parse site.
BEZEL_TEST(radix_validity_7268, unadmitted_radix_at_parse_raises_argument_exception) {
    expectRaisedArgumentException(runAot(parseProg("3")));
}

// The TryParse site.
BEZEL_TEST(radix_validity_7268, unadmitted_radix_at_tryparse_raises_argument_exception) {
    expectRaisedArgumentException(runAot(tryParseProg("3")));
}

// ─── THE SILENT-DECIMAL ROWS (the rendering sites) ─────────────────────────
//
// On the pre-fix arm these printed the DECIMAL text "42" — a silently wrong
// answer in a program that compiles.

// The primitive receiver's `ToString(radix)`.
BEZEL_TEST(radix_validity_7268, unadmitted_radix_at_tostring_raises) {
    // stdlib §9.1: "`ToString(radix)` … takes the same radix set, raising
    // `ArgumentException` for any other".
    expectRaisedArgumentException(runAot(toStringProg("42", "3")));
}

// The same site at the far end of the alphabet.
BEZEL_TEST(radix_validity_7268, radix_36_at_tostring_raises) {
    expectRaisedArgumentException(runAot(toStringProg("42", "36")));
}

// `Convert.ToString(value, toBase)` — stdlib §2.3's "bases 2, 8, 10, 16;
// ArgumentException otherwise".
BEZEL_TEST(radix_validity_7268, unadmitted_tobase_at_convert_raises) {
    expectRaisedArgumentException(runAot(convertProg("3")));
}

// The same site at the far end of the alphabet.
BEZEL_TEST(radix_validity_7268, radix_36_at_convert_raises) {
    expectRaisedArgumentException(runAot(convertProg("36")));
}

// ─── THE SILENT-WRONG-PARSE ROWS ──────────────────────────────────────────
//
// On the pre-fix arm these parsed in the C library's base and printed the
// result — `int.Parse("11", 36)` printed "37", `int.Parse("11", 3)` printed
// "4" — with no `ArgumentException` at all.

BEZEL_TEST(radix_validity_7268, radix_36_at_parse_raises) {
    expectRaisedArgumentException(runAot(parseProg("36")));
}

BEZEL_TEST(radix_validity_7268, radix_3_at_parse_raises) {
    expectRaisedArgumentException(runAot(parseProg("3")));
}

BEZEL_TEST(radix_validity_7268, radix_3_at_tryparse_raises) {
    // stdlib §9.1: "an invalid radix raises ArgumentException from both
    // forms" — the `TryParse` form included.
    expectRaisedArgumentException(runAot(tryParseProg("3")));
}

// ─── THE SPELLINGS ────────────────────────────────────────────────────────
//
// A validity check that rejects only a LITERAL is not the rule: the radix
// reaches the parameter as an `int` however it is spelled, so every spelling
// of an unadmitted radix must raise, and every spelling of an admitted one
// must still work.

// A `uint` CONSTANT radix — §5.1's constant narrowing admits it at the `int`
// parameter, so it BINDS, and its VALUE is outside the admitted set.
BEZEL_TEST(radix_validity_7268, uint_constant_radix_raises) {
    expectRaisedArgumentException(runAot(parseProg("3U")));
}

// A `ulong` CONSTANT radix — the same rule, the other width.
BEZEL_TEST(radix_validity_7268, ulong_constant_radix_raises) {
    expectRaisedArgumentException(runAot(parseProg("3UL")));
}

// A FOLDABLE constant expression (§17.14) — `1 + 2` is 3.
BEZEL_TEST(radix_validity_7268, foldable_constant_radix_raises) {
    expectRaisedArgumentException(runAot(parseProg("1 + 2")));
}

// A RUNTIME `int` radix — a value the compiler cannot fold.
BEZEL_TEST(radix_validity_7268, runtime_int_radix_raises) {
    expectRaisedArgumentException(runAot(runtimeRadixProg(
        "      _ := int.Parse(\"ff\", R());\n", 3)));
}

// A runtime radix at the rendering site.
BEZEL_TEST(radix_validity_7268, runtime_int_radix_at_tostring_raises) {
    expectRaisedArgumentException(runAot(runtimeRadixProg(
        "      _ := (42).ToString(R());\n", 3)));
}

// A runtime radix at the `Convert.ToString` site.
BEZEL_TEST(radix_validity_7268, runtime_int_radix_at_convert_raises) {
    expectRaisedArgumentException(runAot(runtimeRadixProg(
        "      _ := Convert.ToString(42, R());\n", 3)));
}

// ─── THE OTHER INTEGER RECEIVERS ──────────────────────────────────────────
//
// stdlib §9.1: "The example below shows `Int32`; the others have analogous
// shapes" — so the rule reaches every integer type's `Parse(s, radix)` and
// `ToString(radix)`.

BEZEL_TEST(radix_validity_7268, byte_parse_unadmitted_radix_raises) {
    expectRaisedArgumentException(runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      _ := byte.Parse(\"ff\", 3);\n"
        "      return 0;\n"
        "    catch\n"
        "      ArgumentException => return 1;\n"
        "    end;\n"
        "  end Main;\n"
        "end P;\n"));
}

BEZEL_TEST(radix_validity_7268, long_parse_unadmitted_radix_raises) {
    expectRaisedArgumentException(runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      _ := long.Parse(\"ff\", 3);\n"
        "      return 0;\n"
        "    catch\n"
        "      ArgumentException => return 1;\n"
        "    end;\n"
        "  end Main;\n"
        "end P;\n"));
}

BEZEL_TEST(radix_validity_7268, long_receiver_tostring_unadmitted_radix_raises) {
    expectRaisedArgumentException(runAot(toStringProg("255L", "3")));
}

// ─── THE CONTROLS — the ADMITTED radices must not move ────────────────────
//
// §9.1: "The two are therefore inverse: `T.Parse(n.ToString(radix), radix)`
// is `n` for every value of every integer type and every admitted radix."

BEZEL_TEST(radix_validity_7268, hex_radix_at_parse_still_parses) {
    // stdlib §9.1: `int.Parse("ff", 16)` is 255.
    auto r = runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    return int.Parse(\"ff\", 16);\n"
        "  end Main;\n"
        "end P;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 255);
}

BEZEL_TEST(radix_validity_7268, binary_radix_at_parse_still_parses) {
    auto r = runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    return int.Parse(\"1010\", 2);\n"
        "  end Main;\n"
        "end P;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

BEZEL_TEST(radix_validity_7268, hex_radix_at_tryparse_still_parses) {
    auto r = runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var v: int := 0;\n"
        "    _ := int.TryParse(\"ff\", 16, out v);\n"
        "    return v;\n"
        "  end Main;\n"
        "end P;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 255);
}

// The `uint` CONSTANT radix — §5.1's constant narrowing admits it at the
// `int` parameter and 16 is an ADMITTED radix, so it must still PARSE.
BEZEL_TEST(radix_validity_7268, uint_constant_hex_radix_still_parses) {
    auto r = runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    return int.Parse(\"ff\", 16U);\n"
        "  end Main;\n"
        "end P;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 255);
}

// The ADMITTED radix spelled as a foldable constant expression still works.
BEZEL_TEST(radix_validity_7268, foldable_constant_hex_radix_still_parses) {
    auto r = runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    return int.Parse(\"ff\", 8 + 8);\n"
        "  end Main;\n"
        "end P;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 255);
}

// The rendering controls: 2, 8, 10 and 16 render their own digits.
BEZEL_TEST(radix_validity_7268, admitted_radices_at_tostring_still_render) {
    // stdlib §9.1: "`ToString(radix)` emits the **lower-case** digits".
    auto r = runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine((42).ToString(2));\n"
        "    Console.WriteLine((42).ToString(8));\n"
        "    Console.WriteLine((42).ToString(10));\n"
        "    Console.WriteLine((42).ToString(16));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("101010\n52\n42\n2a\n"));
}

// The `Convert.ToString` control: §2.3 "returns the text
// `value.ToString(toBase)` returns".
BEZEL_TEST(radix_validity_7268, admitted_tobase_at_convert_still_renders) {
    auto r = runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Convert.ToString(42, 16));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2a\n"));
}

// The INVERSE property of §9.1, over the admitted radices.
BEZEL_TEST(radix_validity_7268, parse_and_tostring_are_inverse_over_admitted_radices) {
    // stdlib §9.1: "T.Parse(n.ToString(radix), radix) is n for every value of
    // every integer type and every admitted radix".
    auto r = runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(int.Parse((255).ToString(16), 16));\n"
        "    Console.WriteLine(int.Parse((255).ToString(2), 2));\n"
        "    Console.WriteLine(int.Parse((255).ToString(8), 8));\n"
        "    Console.WriteLine(int.Parse((255).ToString(10), 10));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("255\n255\n255\n255\n"));
}
