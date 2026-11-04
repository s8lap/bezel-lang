// Regression tests for #7340 — the JSON deserializer's NUMBER GRAMMAR
// (stdlib §2.15, v0.355).
//
// THE SPEC SENTENCE.  stdlib §2.15 (docs/Bezel_Standard_Library.md:953), the
// "JSON mapping" paragraph:
//
//   "A *JSON number* is JSON's own number token (RFC 8259 §6): an optional
//   `-`, an integer part that is `0` or a nonzero digit followed by zero or
//   more digits, an optional fraction — a `.` and one or more digits — and an
//   optional exponent — `e` or `E`, an optional sign, and one or more digits.
//   Any other spelling of a number — `.5`, `5.`, `1.e3`, `+5`, `007`, `1_000`,
//   `0x1p3` — is malformed input (**Errors** below)."
//
// and stdlib §2.15 (docs/Bezel_Standard_Library.md:1073), "Errors":
//
//   "**Errors.** Malformed input, type mismatches (a `ReadInt` when the next
//   token is a string), and depth-limit violations raise
//   `SerializationException` (declared in `System.Serialization`)."
//
// and stdlib §2.15's `WriteInt`/`WriteUInt`; `ReadInt`/`ReadUInt` row
// (docs/Bezel_Standard_Library.md:957):
//
//   "`ReadInt` and `ReadUInt` read only such a number, so one with a fraction
//   or an exponent (`5.0`, `1e3`, `5.9`) is a type mismatch and raises
//   `SerializationException` located at the number, whatever its value"
//
// and stdlib §2.15's `WriteDecimal`; `ReadDecimal` row
// (docs/Bezel_Standard_Library.md:959):
//
//   "`ReadDecimal` reads only a JSON number with no exponent, of any length:
//   one with an exponent (`1e3`, `1.5E-2`, `1e0`) is a type mismatch and
//   raises `SerializationException` located at the number, whatever its value,
//   as at `ReadInt`"
//
// and stdlib §2.15's `WriteFloat`; `ReadFloat` row
// (docs/Bezel_Standard_Library.md:958):
//
//   "`ReadFloat` reads a JSON number of any length, its value rounded once to
//   `double` as §9.2 rounds `double.Parse`'s input"
//
// THE DEFECT.  The four number readers in
// `src/runtime/bezel_runtime_serialization.cpp` each had their own ad-hoc
// scan, and every one of them was a SUFFIX test or nothing at all:
//
//   * `bz_jder_parse_float` decided the shape of the token by looking at its
//     LAST character, so it admitted a bare point (".5"), a leading "+"
//     ("+.5"), a leading zero followed by a digit ("01") and any non-numeric
//     junk ("nope", "}") as a number, reading them as 0 — and, when the token
//     carried NO digits at all, it read ONE BYTE BEFORE ITS BUFFER (the token
//     length was 0, so `buf[len - 1]` was `buf[-1]`);
//   * `bz_jder_parse_int` / `bz_jder_parse_uint` admitted a leading zero
//     followed by a digit ("01" read as 1);
//   * `bz_jder_parse_decimal` never scanned an exponent at all, so "1e3" read
//     as 1, and it never checked the parse's result, so an out-of-range value
//     read as 0;
//   * the unknown-field SKIP path accepted any digit soup as a number.
//
// THE FIX.  ONE scanner (bz_jder_scan_number) implements RFC 8259 §6's number
// token, and every number read — int, uint, float, decimal — goes through it.
// A token that is not a JSON number is malformed input and raises
// SerializationException at the number; a token that IS a JSON number but does
// not fit the reader's type is a type mismatch and raises the same exception,
// at the number.
//
// THE ORACLE.  Every row below asserts that the program RAN (`runAot` reports
// `ran == false` when the COMPILER rejected the program, which is a different
// outcome from "the program ran and raised"), the program's EXIT STATUS, and
// the program's EXACT stdout.  The exit status is the number of reads that
// raised, so it says WHICH path ran; the text carries the value and the
// exception kind, because a process status is masked to 0..255 and cannot
// carry them.
//
// The rows are on `runAot` (NOT `runAotChecked`), except the ONE row that
// exists to pin the memory-safety half of the defect: a SerializationException
// is a RUNTIME outcome, observable from the exit status and the stdout, and
// needs no sanitizer — but the OUT-OF-BOUNDS READ does, so that row links the
// ASan-INSTRUMENTED runtime (`runUnderAsanExitCode` -> `asanRuntimeObject()`),
// because the read is in the RUNTIME and only the instrumented runtime can see
// it.  `runAotChecked` links the PLAIN runtime archive, so it is kept for the
// other row (the generated-code ASan/LSan leg).
//
// THE CONTROL ROWS are §2.15's own JSON numbers, and they must be byte-
// identical on the pre-fix and the post-fix arm: a reader that admits a
// malformed number is broken, but a reader that REJECTS a well-formed one is
// broken too.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/run_under_asan.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// One measured field value: the JSON TEXT of the value, and the rendering the
// read must produce.  An empty `value` means the read must RAISE
// SerializationException.
struct Case {
    std::string number;
    std::string value;
};

// THE DEFECT SET — every spelling stdlib §2.15 names as malformed, plus the
// shapes the issue names.  Every one of them must raise SerializationException.
//   "nope"   — not a number at all;
//   ""       — an EMPTY value (the field has none);
//   "+.5"    — a leading '+' (JSON has no sign but '-');
//   ".5"     — a bare point (no digit before it);
//   "1."     — an empty fraction;
//   "01"     — a leading zero followed by a digit;
//   "-"      — no integer part;
//   "1e"     — an empty exponent;
//   "--1"    — two signs;
//   "1_000"  — a digit separator (and trailing junk after the number).
const std::vector<Case> kMalformed = {
    {"nope", ""}, {"", ""},   {"+.5", ""}, {".5", ""}, {"1.", ""},
    {"01", ""},   {"-", ""},  {"1e", ""},   {"--1", ""}, {"1_000", ""},
};

// THE CONTROL SET — well-formed JSON numbers, with the value each read must
// produce.  `double`'s rendering is stdlib §2.18's shortest round-trip form
// ("1000", not "1000.0"), `decimal`'s is §9.3's exact form.
const std::vector<Case> kControl = {
    {"1.5", "1.5"},      {"-0", "-0"},      {"1e3", "1000"},
    {"1E-2", "0.01"},    {"0", "0"},        {"-12.25e+1", "-122.5"},
};

// The JSON numbers an INTEGER reader admits: no fraction, no exponent.
const std::vector<Case> kIntegerControl = {
    {"-0", "0"},
    {"0", "0"},
};

// The JSON numbers an INTEGER reader must REJECT as a TYPE MISMATCH: they are
// well-formed JSON numbers, but they carry a fraction or an exponent, and
// §2.15's ReadInt/ReadUInt row says such a number "is a type mismatch and
// raises SerializationException located at the number, whatever its value".
const std::vector<Case> kIntegerTypeMismatch = {
    {"1.5", ""}, {"1e3", ""}, {"1E-2", ""}, {"-12.25e+1", ""},
};

// The JSON numbers a DECIMAL reader must REJECT as a TYPE MISMATCH: §2.15's
// ReadDecimal row names "1e3", "1.5E-2" and "1e0" itself.
const std::vector<Case> kDecimalTypeMismatch = {
    {"1e3", ""}, {"1.5E-2", ""}, {"1e0", ""},
};

// The JSON numbers a DECIMAL reader admits (§9.3's rendering: no exponent, no
// trailing zeros).
const std::vector<Case> kDecimalControl = {
    {"1.5", "1.5"}, {"0", "0"}, {"-12.25", "-12.25"},
};

// The JSON text of a field's value.
std::string jsonOf(const std::string& number) { return "{\"V\": " + number + "}"; }

// A Bezel string literal holding `text`.
std::string lit(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// The expected stdout of a read program over a table: one line per input.
std::string wantStdout(const std::vector<Case>& cases) {
    std::string want;
    for (const Case& c : cases) {
        want += jsonOf(c.number) + " -> " +
                (c.value.empty() ? std::string("SER") : "OK " + c.value) + "\n";
    }
    return want;
}

// The exit status of a read program over a table: the number of reads that
// raised.
int wantExit(const std::vector<Case>& cases) {
    int n = 0;
    for (const Case& c : cases)
        if (c.value.empty()) ++n;
    return n;
}

// The `Show<tag>` method for the record whose name is `rec`.
std::string showMethod(const std::string& tag, const std::string& rec) {
    return "  private static method Show" + tag + "(json: string): int\n"
           "    using var d := new JsonDeserializer(json);\n"
           "    try\n"
           "      var v := " + rec + ".Deserialize(d);\n"
           "      Console.WriteLine(json + \" -> OK \" + v.V.ToString());\n"
           "      return 0;\n"
           "    catch\n"
           "      var e: SerializationException => do Console.WriteLine(json + "
           "\" -> SER\"); return 1; end\n"
           "    end;\n"
           "  end Show" + tag + ";\n";
}

// A program that reads each input through a record whose field has the given
// type, and prints "<json> -> OK <value>" (the read succeeded) or
// "<json> -> SER" (a SerializationException was caught).  Its exit status is
// the number of reads that raised.
//
// Only SerializationException is caught, so a read that raises anything else
// aborts the program and the row's stdout assertion fails — the exception
// TYPE is part of the contract, not just "something was raised".
std::string readProgram(const std::string& type,
                         const std::vector<Case>& cases) {
    const std::string tag = (type == "double") ? "D"
                             : (type == "int")  ? "I"
                             : (type == "long") ? "L"
                                                 : "M";
    const std::string rec = "R" + tag;
    std::string body;
    for (const Case& c : cases)
        body += "    total := total + Show" + tag + "(" + lit(jsonOf(c.number)) +
                ");\n";
    return "module Conf;\n"
           "import System.Serialization;\n"
           "record " + rec + "(V: " + type + "): ISerializable, IDeserializable;\n"
           "public class Program\n"
           "  public static method Main(): int\n"
           "    var total: int := 0;\n" + body +
           "    return total;\n"
           "  end Main;\n" +
           showMethod(tag, rec) +
           "end Program;\n";
}

}  // namespace

// ===========================================================================
// THE DEFECT — every malformed number raises SerializationException, for
// every field type.
// ===========================================================================

// stdlib §2.15: "Any other spelling of a number — `.5`, `5.`, `1.e3`, `+5`,
// `007`, `1_000`, `0x1p3` — is malformed input", and "Malformed input ...
// raise `SerializationException`".
BEZEL_TEST(json_number_grammar_7340,
           json_double_rejects_every_malformed_number_spec_2_15) {
    JitResult r = runAot(readProgram("double", kMalformed));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantStdout(kMalformed));
    BEZEL_EXPECT_EQ(r.exitCode, wantExit(kMalformed));
}

// The same grammar, read into an `int` field.
BEZEL_TEST(json_number_grammar_7340,
           json_int_rejects_every_malformed_number_spec_2_15) {
    JitResult r = runAot(readProgram("int", kMalformed));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantStdout(kMalformed));
    BEZEL_EXPECT_EQ(r.exitCode, wantExit(kMalformed));
}

// The same grammar, read into a `long` field.
BEZEL_TEST(json_number_grammar_7340,
           json_long_rejects_every_malformed_number_spec_2_15) {
    JitResult r = runAot(readProgram("long", kMalformed));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantStdout(kMalformed));
    BEZEL_EXPECT_EQ(r.exitCode, wantExit(kMalformed));
}

// The same grammar, read into a `decimal` field.
BEZEL_TEST(json_number_grammar_7340,
           json_decimal_rejects_every_malformed_number_spec_2_15) {
    JitResult r = runAot(readProgram("decimal", kMalformed));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantStdout(kMalformed));
    BEZEL_EXPECT_EQ(r.exitCode, wantExit(kMalformed));
}

// ===========================================================================
// THE TYPE MISMATCH — a well-formed JSON number that does not fit the
// reader's type raises the SAME exception, at the number.
// ===========================================================================

// stdlib §2.15: "ReadInt and ReadUInt read only such a number, so one with a
// fraction or an exponent (`5.0`, `1e3`, `5.9`) is a type mismatch and
// raises `SerializationException` located at the number, whatever its value".
BEZEL_TEST(json_number_grammar_7340,
           json_int_rejects_a_fraction_or_an_exponent_as_a_type_mismatch_spec_2_15) {
    JitResult r = runAot(readProgram("int", kIntegerTypeMismatch));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantStdout(kIntegerTypeMismatch));
    BEZEL_EXPECT_EQ(r.exitCode, wantExit(kIntegerTypeMismatch));
}

// The same, read into a `long` field.
BEZEL_TEST(json_number_grammar_7340,
           json_long_rejects_a_fraction_or_an_exponent_as_a_type_mismatch_spec_2_15) {
    JitResult r = runAot(readProgram("long", kIntegerTypeMismatch));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantStdout(kIntegerTypeMismatch));
    BEZEL_EXPECT_EQ(r.exitCode, wantExit(kIntegerTypeMismatch));
}

// stdlib §2.15: "`ReadDecimal` reads only a JSON number with no exponent, of
// any length: one with an exponent (`1e3`, `1.5E-2`, `1e0`) is a type mismatch
// and raises `SerializationException` located at the number".
BEZEL_TEST(json_number_grammar_7340,
           json_decimal_rejects_an_exponent_as_a_type_mismatch_spec_2_15) {
    JitResult r = runAot(readProgram("decimal", kDecimalTypeMismatch));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantStdout(kDecimalTypeMismatch));
    BEZEL_EXPECT_EQ(r.exitCode, wantExit(kDecimalTypeMismatch));
}

// ===========================================================================
// THE CONTROLS — the well-formed JSON numbers still read, with their values.
// ===========================================================================

// stdlib §2.15: "`ReadFloat` reads a JSON number of any length, its value
// rounded once to `double`".
BEZEL_TEST(json_number_grammar_7340,
           json_double_admits_every_valid_json_number_spec_2_15) {
    JitResult r = runAot(readProgram("double", kControl));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantStdout(kControl));
    BEZEL_EXPECT_EQ(r.exitCode, wantExit(kControl));
}

// The integer readers admit the JSON numbers with no fraction and no exponent.
BEZEL_TEST(json_number_grammar_7340,
           json_int_admits_every_valid_integer_json_number_spec_2_15) {
    JitResult r = runAot(readProgram("int", kIntegerControl));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantStdout(kIntegerControl));
    BEZEL_EXPECT_EQ(r.exitCode, wantExit(kIntegerControl));
}

BEZEL_TEST(json_number_grammar_7340,
           json_long_admits_every_valid_integer_json_number_spec_2_15) {
    JitResult r = runAot(readProgram("long", kIntegerControl));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantStdout(kIntegerControl));
    BEZEL_EXPECT_EQ(r.exitCode, wantExit(kIntegerControl));
}

// The decimal reader admits the JSON numbers with no exponent.
BEZEL_TEST(json_number_grammar_7340,
           json_decimal_admits_every_valid_json_number_spec_2_15) {
    JitResult r = runAot(readProgram("decimal", kDecimalControl));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantStdout(kDecimalControl));
    BEZEL_EXPECT_EQ(r.exitCode, wantExit(kDecimalControl));
}

// ===========================================================================
// THE MEMORY-SAFETY HALF — the pre-fix float scanner read ONE BYTE BEFORE
// ITS BUFFER when the token carried no digits at all (the token length was 0,
// so `buf[len - 1]` was `buf[-1]`).
//
// THE ORACLE.  The read is in the RUNTIME, so the row must link an
// ASan-INSTRUMENTED runtime: `runAotChecked` links the PLAIN runtime archive
// (Driver::linkCommandLine adds only `-fsanitize=address` to the LINK line,
// which instruments the GENERATED code, not libbezel_runtime.a), so a
// runtime-side read is invisible to it.  `runUnderAsanExitCode` links
// `asanRuntimeObject()` — the runtime compiled with `-fsanitize=address` —
// and is the only one of the three families here that can see the read.
//
// Its return contract: -1 = the toolchain is unavailable (SKIP), -2/-3/-4 =
// the harness's own build/link/exec failures (it marks the test failed and
// prints an [ INFRA ] marker for those), 99 = the sanitizer reported a
// problem OR the program crashed, and otherwise the PROGRAM'S OWN exit code.
// The program returns the number of reads that raised, so a clean run under
// ASan returns that same number.
// ===========================================================================

BEZEL_TEST(json_number_grammar_7340,
           json_number_scan_under_asan_has_no_out_of_bounds_read_spec_2_15) {
    // The three inputs whose pre-fix scan consumed NO digit at all: the
    // token length was 0, so the reader indexed buf[-1].
    const std::vector<Case> kNoDigit = {
        {"nope", ""}, {"", ""}, {"+.5", ""},
    };
    const int rc = bezel::test::runUnderAsanExitCode(
        readProgram("double", kNoDigit), "json-7340-asan");
    // -1 = toolchain unavailable (SKIP the leg; under BEZEL_AOT_FORCE_SANITIZE
    // the shared helper fails the test instead of returning -1).
    if (rc != -1) {
        // 99 = the sanitizer fired (the pre-fix out-of-bounds read) or the
        // program crashed.  It must NOT fire.
        BEZEL_EXPECT_TRUE(rc != 99);
        BEZEL_EXPECT_EQ(rc, wantExit(kNoDigit));
    }
}

// The same reads under the GENERATED-CODE ASan/LSan leg: the malformed
// numbers raise, and nothing leaks (the fix allocates the number's text on
// the heap when it is longer than the stack buffer).
BEZEL_TEST(json_number_grammar_7340,
           json_malformed_number_read_under_asan_is_clean_spec_2_15) {
    JitResult r = runAotChecked(readProgram("double", kMalformed));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantStdout(kMalformed));
    BEZEL_EXPECT_EQ(r.exitCode, wantExit(kMalformed));
}
