// Regression tests for issue #2348 — stdlib §2.4's `Environment` read/write
// family was UNIMPLEMENTED under its current spelling, so every call compiled,
// exited 0, and produced a zero-initialized (or no) value.
//
// stdlib §2.4 (v0.307):
//
//     public static method Get(name: string): string?;    // null when absent
//     public static method GetInt(name: string): int?;    // convenience tier:
//     public static method GetBool(name: string): bool?;  //   null when absent
//                                                         //   OR malformed
//     public static method Set(name: string, value: string?);
//         // value = null removes the variable; an empty name, or one
//         //   containing "=", raises ArgumentException
//
// MEASURED pre-fix with the real `build/bezelc` (three distinct silent shapes):
//
//   var v := Environment.Get("HOME");       -> compiles, exit 0, v is NULL
//        warning: internal: lowerLocalDecl's initializer expression failed to
//        lower to a value for this local's slot (degraded to a zero-initialized
//        value) — #2434
//        ...and `Console.WriteLine(v ?? "<null>")` printed "<null>" for a
//        variable that IS set. Only the v0.231-RENAMED-AWAY spelling
//        (`GetEnvironmentVariable`) had a codegen arm; §2.4 declares `Get`.
//
//   var n := Environment.GetInt("X");       -> same #2434 degrade; GetInt and
//   var b := Environment.GetBool("X");         GetBool (added at v0.215) had no
//                                              arm under any spelling.
//
// ⚠ SCOPE: this commit closes `Get` and `Set` only. `GetInt`/`GetBool` return
// the nullable-primitive companion struct `{i8 hasValue, T value}`, which
// downstream code can only consume once SEMA types the call `int?` / `bool?`;
// that typing belongs in the static-builtin-class accept-list in
// `src/sema/ExprTypecheck_Calls.cpp`, a file held by another lane this cycle.
// Landing the lowering without the typing emits MALFORMED IR (measured:
// `Trunc only operates on integer  %46 = trunc { i8, i1 } %envg.result to i1`),
// so the two halves must land together and are deferred as one unit.
//
//   Environment.Set("K", "v");              -> NO DIAGNOSTIC AT ALL. A
//                                              unit-returning STATEMENT call
//                                              that reaches no lowering arm
//                                              leaves no local to degrade, so
//                                              not even the #2434 note fires;
//                                              the statement simply vanishes.
//
// Every oracle below RUNS and reads stdout or an exception: a compile-only
// assertion cannot tell "works" from "emits nothing", which is the entire
// point of this defect class (#3079 / #2434).
//
// Hermetic by construction: each test program is its own forked process, so
// `Set` mutates only that process's environment and the variable names are
// prefixed `BZ_T_2348_` to avoid colliding with the harness's own.
//
// Cite: stdlib §2.4, §9.4 (GetBool's accepted forms), §9.1 (the int domain),
// language spec §31.4 (ArgumentException), v0.307.

#include "support/aot_support.hpp"  // runAot, runAotOut
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module Conf;\npublic class Program\n"
           "  public static method Main()\n" + body +
           "  end Main;\nend Program;\n";
}

}  // namespace

// ═══════════ §2.4 Get — the v0.231 rename retarget ════════════════════════
// Pre-fix: printed "<null>" (the #2434 zero-init) for a variable just set.
BEZEL_TEST(environment_get_family_2348, get_reads_back_a_set_value_stdlib_2_4) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_2348_A\", \"hello\");\n"
        "    var v := Environment.Get(\"BZ_T_2348_A\");\n"
        "    Console.WriteLine((v ?? \"<null>\"));\n");
    BEZEL_EXPECT_EQ(runAotOut(p), std::string("hello"));
}

// §2.4: "null when the variable is absent".
BEZEL_TEST(environment_get_family_2348, get_of_absent_variable_is_null_stdlib_2_4) {
    const std::string p = prog(
        "    var v := Environment.Get(\"BZ_T_2348_DEFINITELY_ABSENT\");\n"
        "    Console.WriteLine(v = null);\n");
    BEZEL_EXPECT_EQ(runAotOut(p), std::string("True"));
}

// §2.4: "value = null removes the variable".
BEZEL_TEST(environment_get_family_2348, set_null_removes_the_variable_stdlib_2_4) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_2348_R\", \"present\");\n"
        "    Console.WriteLine((Environment.Get(\"BZ_T_2348_R\") ?? \"<null>\"));\n"
        "    Environment.Set(\"BZ_T_2348_R\", null);\n"
        "    Console.WriteLine((Environment.Get(\"BZ_T_2348_R\") ?? \"<null>\"));\n");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("present\n<null>\n"));
}

// ═══════════ §2.4 Set's band-1 argument contract ══════════════════════════
// "an empty name, or one containing "=", raises ArgumentException"
// (language spec §31.4). Pre-fix the call vanished entirely, so neither
// name was rejected and the catch arm never ran.
BEZEL_TEST(environment_get_family_2348, set_empty_name_raises_argumentexception_stdlib_2_4) {
    const std::string p = prog(
        "    try\n"
        "      Environment.Set(\"\", \"v\");\n"
        "      Console.WriteLine(\"no-throw\");\n"
        "    catch e: ArgumentException =>\n"
        "      Console.WriteLine(\"caught\");\n"
        "    end;\n");
    BEZEL_EXPECT_EQ(runAotOut(p), std::string("caught"));
}

BEZEL_TEST(environment_get_family_2348, set_name_with_equals_raises_argumentexception_stdlib_2_4) {
    const std::string p = prog(
        "    try\n"
        "      Environment.Set(\"BZ_T=2348\", \"v\");\n"
        "      Console.WriteLine(\"no-throw\");\n"
        "    catch e: ArgumentException =>\n"
        "      Console.WriteLine(\"caught\");\n"
        "    end;\n");
    BEZEL_EXPECT_EQ(runAotOut(p), std::string("caught"));
}

// Control: a well-formed name does NOT raise — the guard must not fire on the
// ordinary path (a §18 over-rejection control for the new throw).
BEZEL_TEST(environment_get_family_2348, set_well_formed_name_does_not_raise_stdlib_2_4) {
    const std::string p = prog(
        "    try\n"
        "      Environment.Set(\"BZ_T_2348_OK\", \"v\");\n"
        "      Console.WriteLine((Environment.Get(\"BZ_T_2348_OK\") ?? \"<null>\"));\n"
        "    catch e: ArgumentException =>\n"
        "      Console.WriteLine(\"caught\");\n"
        "    end;\n");
    BEZEL_EXPECT_EQ(runAotOut(p), std::string("v"));
}

// ═══════════ Sema typing — the composed forms §2.4's own idioms use ═══════
// stdlib §1 / changelog v0.231 gives the canonical one-liner
// `int.ParseOrNull(Environment.Get("PORT") ?? "") ?? 8080`; the `??` on a
// `string?` only composes if sema types `Get` as `string?`. Pre-fix the call
// carried NO exprType at all.
BEZEL_TEST(environment_get_family_2348, get_result_composes_with_coalesce_stdlib_1) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_2348_P\", \"8080\");\n"
        "    var port := int.Parse(Environment.Get(\"BZ_T_2348_P\") ?? \"1\");\n"
        "    Console.WriteLine(port);\n");
    BEZEL_EXPECT_EQ(runAotOut(p), std::string("8080"));
}

// The `.Length` read is only reachable if the `string?` is narrowed by the
// `??`; it also proves the returned value is a real heap string, not the
// zero-init pointer the pre-fix path produced (which SIGSEGVs here).
BEZEL_TEST(environment_get_family_2348, get_result_is_a_real_string_stdlib_2_4) {
    const std::string p = prog(
        "    Environment.Set(\"BZ_T_2348_S\", \"abcde\");\n"
        "    var s := Environment.Get(\"BZ_T_2348_S\") ?? \"\";\n"
        "    Console.WriteLine(s.Length);\n"
        "    Console.WriteLine(s + \"!\");\n");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\nabcde!\n"));
}

// ═══════════ Control — the §2.4 members that ALREADY worked ══════════════
// `NewLine` / `ProcessorCount` have had a lowering since before this fix
// (LlvmBackend_Exprs.cpp's `Environment` identifier arm); the new sema
// accept-list must type them without disturbing them.
BEZEL_TEST(environment_get_family_2348, newline_and_processorcount_control_stdlib_2_4) {
    const std::string p = prog(
        "    Console.WriteLine(Environment.NewLine.Length);\n"
        "    Console.WriteLine(Environment.ProcessorCount > 0);\n");
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nTrue\n"));
}
