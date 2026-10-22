// Regression tests for #6940 (spec v0.340): §8.4 `=` / `<>` / `Equals` between
// record operands only READS them, so a fresh record construction used as an
// operand is an unbound owned temporary (§28.3 "Unbound owned temporaries",
// v0.340) and its string members must be released at the end of the full
// expression. Before the fix every such temporary leaked its string fields.
//
// ORACLE: runAotChecked (real AOT path, generated-code ASan + LSan). Every
// row loops 200x so a one-shot leak cannot hide, and asserts exact stdout.
// Locals-only rows are the double-free controls: a local operand is not a
// fresh construction and must be left alone.
// NOTE: a record with a class-typed field is not auto-hashable (§9.2/§9.3), so
// `=` on it is rejected by sema; there is no such shape to cover.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
struct Out { int code; std::string text; };
Out run(const std::string& body, const std::string& decls = "") {
    auto r = runAotChecked(
        "module M; record S(N: string) end S; "
        "record P(A: S, T: string, K: int) end P; " + decls +
        " public class Program public static method Main(): int "
        "var c := 0; var d := 0; var loc := new S(\"zz\"); "
        "for var i in 0 ..< 200 do " + body + " end; "
        "Console.WriteLine($\"{c} {d}\"); return 0; end Main; end Program;");
    return {r.ran ? r.exitCode : -999, r.stdoutText};
}
#define ROW(name, body, expected)                                          \
    BEZEL_TEST(record_equality_temporary_release_6940, name) {             \
        auto o = run(body);                                                \
        BEZEL_EXPECT_EQ(o.code, 0);                                        \
        BEZEL_EXPECT_EQ(o.text, std::string(expected));                    \
    }
}  // namespace

// The issue's literal repro (both operands temporaries, if condition).
ROW(spec_8_4_both_temporaries_if_condition,
    "if new S(\"a\" + i.ToString()) = new S(\"b\") then c := c + 1; end;",
    "0 0\n")
ROW(spec_8_4_temporary_vs_local_left,
    "if new S(\"a\" + i.ToString()) = loc then c := c + 1; end;", "0 0\n")
ROW(spec_8_4_temporary_vs_local_right,
    "if loc = new S(\"a\" + i.ToString()) then c := c + 1; end;", "0 0\n")
ROW(spec_8_4_ne_both_temporaries,
    "if new S(\"a\" + i.ToString()) <> new S(\"a\" + i.ToString()) then c := c + 1; end; "
    "if new S(\"a\" + i.ToString()) <> new S(\"b\") then d := d + 1; end;",
    "0 200\n")
ROW(spec_8_4_nested_record_and_string_fields,
    "if new P(new S(\"x\" + i.ToString()), \"t\" + i.ToString(), 1) = "
    "new P(new S(\"x\" + i.ToString()), \"t\" + i.ToString(), 1) then c := c + 1; end;",
    "200 0\n")
ROW(spec_8_4_explicit_equals_both_temporaries,
    "if new S(\"a\" + i.ToString()).Equals(new S(\"a\" + i.ToString())) then c := c + 1; end;",
    "200 0\n")
ROW(spec_8_4_explicit_equals_temporary_receiver_local_arg,
    "if new S(\"a\" + i.ToString()).Equals(loc) then c := c + 1; end;", "0 0\n")
ROW(spec_8_4_var_initializer_temporaries,
    "var r := new S(\"a\" + i.ToString()) = new S(\"a\" + i.ToString()); "
    "if r then c := c + 1; end;",
    "200 0\n")
// Double-free controls: locals only, nothing to release at the comparison.
ROW(spec_8_4_locals_only_control,
    "var l1 := new S(\"m\" + i.ToString()); var l2 := new S(\"m\" + i.ToString()); "
    "if l1 = l1 then c := c + 1; end; if l1 = l2 then d := d + 1; end;",
    "200 200\n")
