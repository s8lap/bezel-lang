// Fix #7320 -- `Guid.TryParse` did not assign `default(Guid)` to its `out` slot
// on the false path (stdlib §1 Try convention, shape (c), §2.6, v0.350).
//
// stdlib §1's table, row (c): `bool` + non-nullable value-typed out,
// `TryX(..., out result: T): bool`, "assigns `default(T)` (spec §22.3)", and the
// row names "the `DateTime`/`DateOnly`/`TimeOnly`/`Guid` `TryParse` members
// (§2.5/§2.6)". The prose: "the out is never null, so `default(T)` (a real value
// distinguished only by the `bool`) is what the false path assigns."
//
// THE DEFECT. `bz_guid_try_parse` returned 0 on a malformed (or null) string
// WITHOUT writing its `out` slot, so the caller's PRE-SET value survived a
// parse that ran and failed. The DateTime / DateOnly / TimeOnly runtimes (and
// `bz_guid_parse`) write the failure default.
//
// THE FIX. The runtime writes the all-zero Guid on every failure path.
//
// DISCRIMINATOR. Every row PRE-SETS the slot to a non-zero Guid. A pre-set to
// the all-zero Guid could not tell "wrote the default" from "never wrote" and
// would pass vacuously. (A call the compiler DROPS -- #7313's non-parseable
// first operand -- is refused at compile time now and never runs; it writes
// nothing and is outside this file.)
//
// ORACLE. `runAotChecked`; each row asserts ran, exit code and exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

const char kPre[]  = "11111111-1111-1111-1111-111111111111";
const char kZero[] = "00000000-0000-0000-0000-000000000000";

// A program that pre-sets `r` to kPre, runs `Guid.TryParse(<arg>, out r)` and
// prints `[ok] [r]`.
std::string tryProg(const std::string& arg) {
    return std::string("module M; import System;\nclass C\n"
                       "  public static method Main(): int\n"
                       "    var r: Guid := Guid.Parse(\"") + kPre + "\");\n"
           "    var ok: bool := Guid.TryParse(" + arg + ", out r);\n"
           "    Console.WriteLine($\"[{ok}] [{r}]\");\n"
           "    return 0;\n"
           "  end Main;\nend C;\n";
}

std::string stdoutOf(const JitResult& r) {
    std::string t = r.stdoutText;
    if (!t.empty() && t.back() == '\n') t.pop_back();
    return t;
}

void expectOut(const std::string& src, const std::string& want) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(stdoutOf(r), want);
}

}  // namespace

// The issue's repro: a malformed string leaves the slot at default(Guid).
BEZEL_TEST(guid_tryparse_false_path_default_7320, invalid_text_writes_default_stdlib_1_shape_c) {
    expectOut(tryProg("\"not-a-valid-value\""),
              std::string("[False] [") + kZero + "]");
}

// A string of the right length whose last hex digit is bad: nothing of the
// partially-decoded value may reach the slot, nor the pre-set value.
BEZEL_TEST(guid_tryparse_false_path_default_7320, bad_digit_right_length_writes_default_stdlib_1_shape_c) {
    expectOut(tryProg("\"22222222-2222-2222-2222-22222222222g\""),
              std::string("[False] [") + kZero + "]");
}

// The empty string, and the 32-character "N" form with a bad digit.
BEZEL_TEST(guid_tryparse_false_path_default_7320, empty_and_bad_n_form_write_default_stdlib_1_shape_c) {
    expectOut(tryProg("\"\""), std::string("[False] [") + kZero + "]");
    expectOut(tryProg("\"2222222222222222222222222222222z\""),
              std::string("[False] [") + kZero + "]");
}

// A `string` computed at run time (not a literal) takes the same path.
BEZEL_TEST(guid_tryparse_false_path_default_7320, runtime_string_writes_default_stdlib_1_shape_c) {
    expectOut(tryProg("\"zz\" + \"zz\""),
              std::string("[False] [") + kZero + "]");
}

// ACCEPT: a valid D-form string returns true and the parsed value.
BEZEL_TEST(guid_tryparse_false_path_default_7320, valid_d_form_assigns_parsed_value_stdlib_2_6) {
    expectOut(tryProg("\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\""),
              "[True] [aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee]");
}

// ACCEPT: a valid N-form string (stdlib §2.6 / §2.18).
BEZEL_TEST(guid_tryparse_false_path_default_7320, valid_n_form_assigns_parsed_value_stdlib_2_6) {
    expectOut(tryProg("\"aaaaaaaabbbbccccddddeeeeeeeeeeee\""),
              "[True] [aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee]");
}

// The `out var` form binds a fresh slot: a failure leaves default(Guid).
BEZEL_TEST(guid_tryparse_false_path_default_7320, out_var_failure_is_default_stdlib_1_shape_c) {
    expectOut(
        "module M; import System;\nclass C\n"
        "  public static method Main(): int\n"
        "    var ok: bool := Guid.TryParse(\"nope\", out var g);\n"
        "    Console.WriteLine($\"[{ok}] [{g}]\");\n"
        "    return 0;\n"
        "  end Main;\nend C;\n",
        std::string("[False] [") + kZero + "]");
}

// A failed parse into a CLASS FIELD slot overwrites the field's old value.
BEZEL_TEST(guid_tryparse_false_path_default_7320, field_out_slot_writes_default_stdlib_1_shape_c) {
    expectOut(
        std::string("module M; import System;\nclass H\n"
                    "  public G: Guid := Guid.Parse(\"") + kPre + "\");\n"
        "end H;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var h := new H();\n"
        "    var ok: bool := Guid.TryParse(\"nope\", out h.G);\n"
        "    Console.WriteLine($\"[{ok}] [{h.G}]\");\n"
        "    return 0;\n"
        "  end Main;\nend C;\n",
        std::string("[False] [") + kZero + "]");
}

// CONTROL (acceptance criterion 3): DateTime / DateOnly / TimeOnly TryParse
// keep writing their failure default.
BEZEL_TEST(guid_tryparse_false_path_default_7320, sibling_receivers_still_write_default_control) {
    expectOut(
        "module M; import System;\nclass C\n"
        "  public static method Main(): int\n"
        "    var d: DateTime := DateTime.Parse(\"2020-01-02T03:04:05\");\n"
        "    var o: DateOnly := DateOnly.Parse(\"2020-01-02\");\n"
        "    var t: TimeOnly := TimeOnly.Parse(\"03:04:05\");\n"
        "    var a: bool := DateTime.TryParse(\"nope\", out d);\n"
        "    var b: bool := DateOnly.TryParse(\"nope\", out o);\n"
        "    var c: bool := TimeOnly.TryParse(\"nope\", out t);\n"
        "    Console.WriteLine($\"[{a}] [{d}] [{b}] [{o}] [{c}] [{t}]\");\n"
        "    return 0;\n"
        "  end Main;\nend C;\n",
        "[False] [0001-01-01T00:00:00] [False] [0001-01-01] [False] [00:00:00]");
}
