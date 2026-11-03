// Regression test for issue #8014 (spec §5.1, §7, §7.3, §22.6, stdlib §2.5,
// v0.350).
//
// THE DEFECT. `new TimeSpan(...)` is the COMPILER'S OWN INTRINSIC -- the
// #7899 fix added `new TimeSpan(hours, minutes, seconds)` beside the
// one-argument raw-tick form -- and stdlib §2.5 (v0.350) declares NO
// `TimeSpan` constructor at all. The ONE built-in parameter table therefore
// carries NO row for it, and the §5.1 admission gate asked NO question at the
// argument positions.
//
// MEASURED on the tree before the fix:
//
//   `new TimeSpan(1.0)`        -> "codegen error: IR verification failed"
//   `new TimeSpan("x", 2, 3)`  -> "codegen error: IR verification failed"
//   `new TimeSpan(1, null, 3)` -> "codegen error: IR verification failed"
//   `new TimeSpan(1, true, 3)` -> COMPILED AND RAN WRONG: it rendered
//                                  `00:59:03` instead of `01:00:03`, because
//                                  the `bool` was sign-extended i1 -> -1.
//
// A VERIFIER ABORT IS NOT A DIAGNOSTIC (the compiler crashed on the user's
// program instead of refusing the argument at its source location), and a
// SILENTLY WRONG VALUE IS WORSE THAN A CRASH.
//
// THE FIX. `ExprTC::checkBuiltinCtorArgTypes` (src/sema/ExprTypecheck_Exprs
// .cpp) now checks the intrinsic's argument positions, positionally, exactly
// as the `JsonOptions` arm checks its five components:
//
//   * the one-argument form's position 0 is the tick count; and
//   * the three-argument form's positions 0, 1 and 2 are the hours, the
//     minutes and the seconds.
//
// The type is `long` at every position: the intrinsic's components are
// INTEGER counts (the compiler computes the span's tick count with integer
// arithmetic -- `h*36_000_000_000 + m*600_000_000 + s*10_000_000`, the same
// tick ratios stdlib §2.5's `From<Unit>` factories use -- and the
// one-argument form is the raw tick count stdlib §2.5 gives the `long`:
// `Ticks: long`, `FromTicks(ticks: long)`). §5.1 admits `int -> long` and
// every other integer type, so every integer literal and every integer-typed
// expression is still admitted, and §5.1 refuses a `double`, a `string` and a
// `bool` ("between integers and `bool` is never implicit"). A nullable
// argument draws §7's own message.
//
// WHY NOT A ROW IN THE ONE TABLE. The table is the §22.5 NAME surface, and
// the table-driven §5.1 gate only considers a form that carries NAMES. A
// name-less row is skipped by that gate, and a row WITH names would admit a
// NAMED argument at a constructor the standard library does not declare --
// a surface the open spec-question #7550 has not settled. So NO name surface
// is invented here: the named spelling keeps its loud "named arguments are
// not supported on this built-in constructor" refusal.
//
// THE ORACLE. `runAot` is the strict AOT oracle (fork-isolated
// Driver::compileFile + clang link + native exec). Every REFUSAL row asserts
// the POSITIVE: the exact §5.1 / §7 diagnostic text, at a REAL source
// location (failedAtCompileTimeWithRealLocation). Every CONTROL row asserts
// the exact stdout the standard library's intrinsic computes. No row asserts
// the absence of a diagnostic.
//
// ASan is OFF in lanes by default (CLAUDE.md): this is an argument's type
// admission and a diagnostic, not allocation, layout, retain/release or
// disposal.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

}  // namespace

namespace {

// The program body is wrapped in a class with a `Main` method.
std::string prog(const std::string& body) {
    return "module M;\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A refusal row: the program must NOT run, the diagnostic must carry `needle`
// at a REAL source location.
void expectRefused(const std::string& body, const char* needle) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    // The diagnostic is LOCATED: the driver's captured stderr carries a
    // `<path>:<line>:<col>:` prefix, so the refusal has a real source
    // position rather than a synthesized one.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// A value row: the program must compile CLEAN, run, exit 0 and print exactly
// this stdout.
void expectStdout(const std::string& body, const std::string& expected) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

// ===========================================================================
// #8014 -- the §5.1 REFUSALS. Each of the issue's own four shapes, and each
// position of the three-argument form.
// ===========================================================================

// The one-argument form: the position is the TICK COUNT, and a `double` is
// never implicitly convertible to a `long` (§5.1). Before the fix this
// reached codegen and ABORTED the IR verifier.
BEZEL_TEST(builtin_timespan_ctor_arg_type_8014,
           a_double_at_the_tick_count_is_refused_5_1) {
    expectRefused("    var ts := new TimeSpan(1.0);\n"
                  "    Console.WriteLine(ts);\n",
                  "the `ticks` argument of the `TimeSpan` constructor has a "
                  "type that is not implicitly convertible to the target type "
                  "(§5.1)");
}

// The three-argument form, position 0: the HOURS.
BEZEL_TEST(builtin_timespan_ctor_arg_type_8014,
           a_string_at_the_hours_is_refused_5_1) {
    expectRefused("    var ts := new TimeSpan(\"x\", 2, 3);\n"
                  "    Console.WriteLine(ts);\n",
                  "the `hours` argument of the `TimeSpan` constructor has a "
                  "type that is not implicitly convertible to the target type "
                  "(§5.1)");
}

// The three-argument form, position 1: the MINUTES.
BEZEL_TEST(builtin_timespan_ctor_arg_type_8014,
           a_string_at_the_minutes_is_refused_5_1) {
    expectRefused("    var ts := new TimeSpan(1, \"x\", 3);\n"
                  "    Console.WriteLine(ts);\n",
                  "the `minutes` argument of the `TimeSpan` constructor has a "
                  "type that is not implicitly convertible to the target type "
                  "(§5.1)");
}

// The three-argument form, position 2: the SECONDS.
BEZEL_TEST(builtin_timespan_ctor_arg_type_8014,
           a_string_at_the_seconds_is_refused_5_1) {
    expectRefused("    var ts := new TimeSpan(1, 2, \"x\");\n"
                  "    Console.WriteLine(ts);\n",
                  "the `seconds` argument of the `TimeSpan` constructor has a "
                  "type that is not implicitly convertible to the target type "
                  "(§5.1)");
}

// The `bool` case -- the sharpest form of the defect, because it produced a
// WRONG VALUE rather than an error: the `bool` was sign-extended i1 -> -1, so
// `new TimeSpan(1, true, 3)` rendered `00:59:03` instead of `01:00:03`.
// §5.1: "between integers and `bool` is never implicit", so the argument is
// refused instead of silently mis-computed.
BEZEL_TEST(builtin_timespan_ctor_arg_type_8014,
           a_bool_at_the_minutes_is_refused_5_1) {
    expectRefused("    var ts := new TimeSpan(1, true, 3);\n"
                  "    Console.WriteLine(ts);\n",
                  "the `minutes` argument of the `TimeSpan` constructor has a "
                  "type that is not implicitly convertible to the target type "
                  "(§5.1): between integers and `bool` is never implicit");
}

// The NULLABLE case -- §7.3: "Assigning `null` to a non-nullable target is
// a compile error". Before the fix the null reached codegen and ABORTED the
// IR verifier ("Both operands to a binary operator are not of the same
// type!").
BEZEL_TEST(builtin_timespan_ctor_arg_type_8014,
           a_null_at_the_minutes_is_refused_7) {
    expectRefused("    var ts := new TimeSpan(1, null, 3);\n"
                  "    Console.WriteLine(ts);\n",
                  "the `minutes` argument of the `TimeSpan` constructor is "
                  "nullable but the target type is non-nullable (§7)");
}

// ===========================================================================
// THE CONTROL ROWS -- the conforming calls still compute the intrinsic's
// value. This is what proves the gate refuses the WRONG argument type and
// not the constructor.
// ===========================================================================

// The three-argument form: 1 hour + 2 minutes + 3 seconds.
BEZEL_TEST(builtin_timespan_ctor_arg_type_8014,
           the_component_form_still_computes_the_span) {
    expectStdout("    var ts := new TimeSpan(1, 2, 3);\n"
                 "    Console.WriteLine(ts);\n",
                 "01:02:03");
}

// The one-argument raw-tick form, byte-identical: 36 000 000 000 ticks is one
// hour (stdlib §2.5: 10 000 000 ticks to the second).
BEZEL_TEST(builtin_timespan_ctor_arg_type_8014,
           the_raw_tick_form_still_computes_the_span) {
    expectStdout("    var ts := new TimeSpan(36000000000L);\n"
                 "    Console.WriteLine(ts);\n",
                 "01:00:00");
}

// A `long` at every component -- §5.1 admits it, so the gate must not
// refuse it.
BEZEL_TEST(builtin_timespan_ctor_arg_type_8014,
           a_long_at_every_component_is_still_admitted) {
    expectStdout("    var ts := new TimeSpan(1L, 2L, 3L);\n"
                 "    Console.WriteLine(ts);\n",
                 "01:02:03");
}

// An UNSIGNED integer at every component -- §5.1 admits it too.
BEZEL_TEST(builtin_timespan_ctor_arg_type_8014,
           an_unsigned_integer_at_every_component_is_still_admitted) {
    expectStdout("    var ts := new TimeSpan(5U, 2U, 3U);\n"
                 "    Console.WriteLine(ts);\n",
                 "05:02:03");
}

// A §7.4-NARROWED nullable read is of its non-nullable type (§22.6), so it
// is ADMITTED at a non-nullable position -- the gate must not refuse a legal
// program.
BEZEL_TEST(builtin_timespan_ctor_arg_type_8014,
           a_narrowed_nullable_read_is_still_admitted_22_6) {
    expectStdout("    var n: int? := 5;\n"
                 "    if n is not null then\n"
                 "      var ts := new TimeSpan(n, 2, 3);\n"
                 "      Console.WriteLine(ts);\n"
                 "    end if;\n",
                 "05:02:03");
}
