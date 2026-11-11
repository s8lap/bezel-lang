// Spec-derived regression test for issue #7419.
//
// spec §22.5 (v0.345) "Optional parameters and named arguments": "Named
// arguments may appear in any order after positional arguments. Each
// supplied argument binds exactly one parameter and each parameter is bound
// by at most one supplied argument ..." — and §22.5's OWN example is a
// REORDERED named call (`Log(level := "ERROR", message := "Failed")`).
//
// A named argument binds to the parameter it NAMES. Three checks read a
// built-in's arguments POSITIONALLY instead, and this test pins the three
// shapes:
//
//   * `StringBuilder.Insert`'s dedicated value check (#6807) read
//     `ce.args[1]` — the INDEX — as the value, so the LEGAL
//     `sb.Insert(value := "X", index := 0)` was refused with "the value must
//     be a `string`" although the value IS a `string`. The `StringBuilder`
//     head honours argument names (it is one of the six heads the §22.5
//     binder claims), and codegen's `Insert` arm consumes the published
//     binding, so the call COMPILES and RUNS correctly.
//   * the shared built-in resolver (`ExprTC::checkBuiltinCallAdmission`)
//     read its arguments positionally, so the LEGAL
//     `Debug.Assert(message := "a message", condition := true)` was refused
//     with a §5.1 type diagnostic that blamed the `condition` parameter for
//     a `string` that is really the MESSAGE (both arguments are correctly
//     typed), and the LEGAL `int.Parse(radix := 16, s := "255")` was
//     refused with a diagnostic naming the `s` parameter.
//   * the same positional reading SILENTLY ADMITTED
//     `int.Parse(radix := "x", s := 5)`, whose named binding is invalid on
//     both parameters.
//
// fix #7602 (§22.5, v0.357): the resolver's heads now BIND a named argument
// to the parameter it NAMES, for the members whose codegen arm reads the
// published §22.5 binding (`SemaContext::setBuiltinCallBinding`), so the
// two reordered calls above COMPILE and RUN and the two refusal rows became
// admitting rows. The members whose arm still lowers its arguments by
// POSITION keep the §22.5 refusal — that refusal is the compiler's
// LIMITATION, not a §5.1 verdict — and it is pinned by
// `builtin_named_arg_reorder_7602_test.cpp`.
//
// The CONTROL rows pin the safety property: every call that binds IN ORDER
// keeps working — `Debug.Assert(condition := true, message := "a message")`,
// `Debug.Assert(condition := true)`, `Debug.WriteLine(message := "x")`,
// `int.Parse(s := "255", radix := 16)`, `int.Parse("255", 16)` and
// `(255).ToString(radix := 16)` — and the POSITIONAL spelling of the
// rejected value (`sb.Insert(1, 5)`) is still rejected.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// fix #7602 (§22.5, v0.357): the shared resolver's heads now BIND a named
// argument to the parameter it NAMES, for the members whose codegen arm reads
// the published binding — so the §22.5 REFUSAL this file used to pin is
// gone for those members, and the rows below are ADMITTING rows. The
// members whose arm still lowers its arguments by POSITION keep the
// refusal, and that refusal is pinned by
// `builtin_named_arg_reorder_7602_test.cpp` (the `int.TryParse` row).

// #6807's own located diagnostic, for a non-`string` value.
const char kInsertValueType[] =
    "no matching overload of `StringBuilder.Insert` — the only overload is "
    "`Insert(index: int, value: string)`, so the value must be a `string`";

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// A rejection row: the program must be REJECTED at compile time, with the
// named diagnostic, at a real source location.
void expectRejected(const std::string& body, const std::string& needle) {
    auto r = runAot(body);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A control row: the program must compile, run clean under the strict AOT
// oracle, and print exactly `out`.
void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAotChecked(body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

std::string prog(const std::string& imports, const std::string& body) {
    return "module M;\n" + imports +
           "class C\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

const char kTextImport[] = "import System.Text;\n";
const char kDiagImport[] = "import System.Diagnostics;\n";

}  // namespace

// ── §22.5: the `StringBuilder` head HONOURS argument names ─────────────────
// (stdlib §3.1 declares `Insert(index: int, value: string): StringBuilder`.)

BEZEL_TEST(builtin_named_arg_binding_7419,
           stringbuilder_insert_named_reordered_is_admitted) {
    expectStdout(prog(kTextImport,
                      "var sb := new StringBuilder(); sb.Append(\"ab\"); "
                      "sb.Insert(value := \"X\", index := 1); "
                      "Console.WriteLine(sb.ToString());"),
                 "aXb\n");
}

// The value is still checked against §3.1's declared `value: string` — and
// the diagnostic now points at the argument that IS the value.
BEZEL_TEST(builtin_named_arg_binding_7419,
           stringbuilder_insert_named_reordered_non_string_value_is_rejected) {
    expectRejected(prog(kTextImport,
                        "var sb := new StringBuilder(); sb.Append(\"ab\"); "
                        "sb.Insert(value := 5, index := 1);"),
                   kInsertValueType);
}

// CONTROL: the POSITIONAL spelling of the same illegal value is still
// rejected — the named spelling did not become a way around §3.1.
BEZEL_TEST(builtin_named_arg_binding_7419,
           stringbuilder_insert_positional_non_string_value_is_rejected) {
    expectRejected(prog(kTextImport,
                        "var sb := new StringBuilder(); sb.Append(\"ab\"); "
                        "sb.Insert(1, 5);"),
                   kInsertValueType);
}

// ── §22.5: the shared resolver's heads cannot honour a REORDERED call ──────

// The issue's own repro: both arguments are correctly typed, and the old
// diagnostic blamed the `condition` parameter for the MESSAGE.
BEZEL_TEST(builtin_named_arg_binding_7419,
           debug_assert_named_reordered_is_admitted) {
    expectStdout(prog(kDiagImport,
                      "Debug.Assert(message := \"a message\", condition := true); "
                      "Console.WriteLine(\"OK\");"),
                 "OK\n");
}

// The `int.Parse` instance of the same class.
BEZEL_TEST(builtin_named_arg_binding_7419,
           int_parse_named_reordered_is_admitted) {
    expectStdout(prog("", "Console.WriteLine(int.Parse(radix := 16, s := \"ff\"));"),
                 "255\n");
}

// The same class's SILENT-ACCEPTANCE instance: the positional reading
// happens to fit, so the call was admitted — and the named binding is
// invalid on BOTH parameters.
BEZEL_TEST(builtin_named_arg_binding_7419,
           int_parse_named_reordered_with_mismatched_types_is_refused) {
    expectRejected(prog("", "var v := int.Parse(radix := \"x\", s := 5);"),
                   "so the `radix` argument must be an `int`");
}

// The §22.5 CANDIDATE SET: a named argument binds to the parameter it
// NAMES, so `ToString(format: string)` is the only candidate a call
// writing `format :=` can reach — the `ToString(radix: int)` form is not a
// candidate for it. Read positionally, the radix form was the one the
// argument fitted, so the call was ADMITTED and rendered "10" (the radix):
// a SILENT WRONG ANSWER for a call §22.5/§5.1 refuse.
BEZEL_TEST(builtin_named_arg_binding_7419,
           int_to_string_named_format_with_an_int_is_rejected) {
    expectRejected(prog("", "Console.WriteLine((255).ToString(format := 16));"),
                   "so the `format` argument must be a `string`");
}

// ── CONTROLS: every call that binds IN ORDER keeps working ─────────────────

BEZEL_TEST(builtin_named_arg_binding_7419,
           debug_assert_named_ordered_is_admitted) {
    expectStdout(prog(kDiagImport,
                      "Debug.Assert(condition := true, message := \"a message\"); "
                      "Console.WriteLine(\"OK\");"),
                 "OK\n");
}

BEZEL_TEST(builtin_named_arg_binding_7419,
           debug_assert_condition_only_is_admitted) {
    expectStdout(prog(kDiagImport,
                      "Debug.Assert(condition := true); "
                      "Console.WriteLine(\"OK\");"),
                 "OK\n");
}

BEZEL_TEST(builtin_named_arg_binding_7419,
           debug_writeline_named_is_admitted) {
    expectStdout(prog(kDiagImport,
                      "Debug.WriteLine(message := \"x\"); "
                      "Console.WriteLine(\"OK\");"),
                 "OK\n");
}

BEZEL_TEST(builtin_named_arg_binding_7419,
           int_parse_named_ordered_is_admitted) {
    expectStdout(prog("", "Console.WriteLine(int.Parse(s := \"255\", radix := 16));"),
                 "597\n");
}

BEZEL_TEST(builtin_named_arg_binding_7419,
           int_parse_positional_is_admitted) {
    expectStdout(prog("", "Console.WriteLine(int.Parse(\"255\", 16));"), "597\n");
}

// The NAMED radix on a primitive receiver — the spelling #7271's rows pin.
BEZEL_TEST(builtin_named_arg_binding_7419,
           int_to_string_named_radix_is_admitted) {
    expectStdout(prog("", "Console.WriteLine((255).ToString(radix := 16));"),
                 "ff\n");
}
