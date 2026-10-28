// Spec-derived regression test for issue #7455 — a REORDERED named BUILT-IN
// CONSTRUCTOR was bound POSITIONALLY (a silent wrong answer).
//
// spec §22.5 "Optional parameters and named arguments" (v0.345); spec §17.7
// "The `new` operator" (v0.345); stdlib §2.5 (v0.345) — `DateTime`,
// `DateOnly`, `TimeOnly`.
//
// WHAT THE DEFECT WAS. §22.5's binder binds each supplied argument to
// exactly one parameter, by NAME for a named argument, and every
// construction site in the language reaches that one binder ("a call, a
// `new` expression, and a record construction alike").  Sema's
// `ExprTC::checkBuiltinCtorNamedArgs` DOES run it for a built-in
// constructor and publishes the parameter -> argument binding through
// `SemaContext::setBuiltinCallBinding`.  Codegen's `lowerNew` arms for the
// §2.5 date/time constructors ignored that binding and read `ne.args[i]`
// POSITIONALLY, so the names were discarded and the wrong fields got the
// wrong values.
//
// MEASURED ON `89756014` (build/bezelc, real AOT compile + native run):
//
//   new DateTime(day := 1, month := 2, year := 2024)  ->  6 8 17   <-- WRONG
//   new DateTime(2024, 2, 1)                         ->  2024 2 1  (correct)
//   new DateOnly(day := 1, month := 2, year := 2024)  ->  6 8 17   <-- WRONG
//   new TimeOnly(minute := 2, hour := 1)              ->  2 1 0 0  <-- WRONG
//
// The rows below are VALUE oracles: they print the constructed value, so a
// positional mis-bind cannot pass.  The reject rows pin the §22.5
// diagnostics that a built-in constructor shares with every other
// construction site.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A whole program whose `Main` body is `body`.  Observables travel on
// stdout, never on the exit status (a process exit status is masked to 8
// bits by `WEXITSTATUS`).
std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A whole program with a user-declared record, for the record-construction
// twin.
std::string progWithRecord(const std::string& body) {
    return "module M; "
           "public record P(x: int, y: int); "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// True iff SOME diagnostic contains `sub`.  Rejection rows assert on
// SPECIFIC message text, never on the coarse `hasErrors()` — `runAot`
// synthesizes an Error diagnostic for a runtime SIGSEGV/ICE too.
bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

// §22.5's own strings, shared with the user/imported call paths.  Asserting
// them is what pins "the SAME binder", not a second implementation.
constexpr const char* kBoundTwiceDiag = "is bound more than once";
constexpr const char* kPositionalAfterNamedDiag =
    "a positional argument may not follow a named argument";
constexpr const char* kSpec22_5 = "§22.5";

}  // namespace

// ---------------------------------------------------------------------------
// The issue's repro — the value oracle that FAILS WITHOUT THE FIX.
// ---------------------------------------------------------------------------

// fix #7455 (§22.5, v0.345): `day`/`month`/`year` bind by NAME.  On the
// pre-fix tree this printed 6 8 17 (the arguments bound positionally as
// DateTime(1, 2, 2024)); §22.5's binding gives 2024 2 1.
BEZEL_TEST(builtin_ctor_named_arg_7455,
           binds_reordered_named_args_on_builtin_datetime_constructor_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var dt := new DateTime(day := 1, month := 2, year := 2024); "
            "    Console.WriteLine($\"{dt.Year} {dt.Month} {dt.Day}\"); ")),
        std::string("2024 2 1"));
}

// The same shape on stdlib §2.5's `DateOnly` (v0.345).
BEZEL_TEST(builtin_ctor_named_arg_7455,
           binds_reordered_named_args_on_builtin_dateonly_constructor_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d := new DateOnly(day := 1, month := 2, year := 2024); "
            "    Console.WriteLine($\"{d.Year} {d.Month} {d.Day}\"); ")),
        std::string("2024 2 1"));
}

// The same shape on stdlib §2.5's `TimeOnly` (v0.345) — whose `second` and
// `millisecond` carry defaults, so the reordered binding also has to leave
// the omitted parameters at their declared defaults.
BEZEL_TEST(builtin_ctor_named_arg_7455,
           binds_reordered_named_args_on_builtin_timeonly_constructor_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var t := new TimeOnly(minute := 2, hour := 1); "
            "    Console.WriteLine($\"{t.Hour} {t.Minute} {t.Second} "
            "{t.Millisecond}\"); ")),
        std::string("1 2 0 0"));
}

// ---------------------------------------------------------------------------
// The twins that must not break (§22.5's rule is unchanged for them).
// ---------------------------------------------------------------------------

// The POSITIONAL spelling — the answer the named spelling must agree with.
BEZEL_TEST(builtin_ctor_named_arg_7455,
           keeps_the_positional_builtin_datetime_constructor_binding) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var dt := new DateTime(2024, 2, 1); "
            "    Console.WriteLine($\"{dt.Year} {dt.Month} {dt.Day}\"); ")),
        std::string("2024 2 1"));
}

// The IN-ORDER named spelling (the binding is the identity).
BEZEL_TEST(builtin_ctor_named_arg_7455,
           keeps_the_in_order_named_builtin_datetime_constructor_binding) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var dt := new DateTime(year := 2024, month := 2, day := 1); "
            "    Console.WriteLine($\"{dt.Year} {dt.Month} {dt.Day}\"); ")),
        std::string("2024 2 1"));
}

// The MIXED spelling: positional arguments first, then named ones — §22.5's
// "named arguments may appear in any order AFTER positional arguments".
BEZEL_TEST(builtin_ctor_named_arg_7455,
           binds_mixed_positional_then_named_builtin_datetime_constructor) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var dt := new DateTime(2024, 2, day := 1); "
            "    Console.WriteLine($\"{dt.Year} {dt.Month} {dt.Day}\"); ")),
        std::string("2024 2 1"));
}

// The DEFAULT-valued parameters: `TimeOnly(hour, minute, second := 0,
// millisecond := 0)` — an omitted parameter keeps its declared default.
BEZEL_TEST(builtin_ctor_named_arg_7455,
           keeps_default_valued_builtin_timeonly_parameters) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var t := new TimeOnly(1, 2); "
            "    Console.WriteLine($\"{t.Hour} {t.Minute} {t.Second} "
            "{t.Millisecond}\"); ")),
        std::string("1 2 0 0"));
}

// A USER type's `new` with named arguments — unchanged (the user path has
// its own binder and must keep it).
BEZEL_TEST(builtin_ctor_named_arg_7455,
           keeps_a_user_types_new_with_named_args_unchanged) {
    BEZEL_EXPECT_EQ(
        runAotOut(progWithRecord(
            "    var p := new P(y := 2, x := 1); "
            "    Console.WriteLine($\"{p.x} {p.y}\"); ")),
        std::string("1 2"));
}

// A RECORD CONSTRUCTION with named arguments — §22.5's "a record
// construction alike" — unchanged.
BEZEL_TEST(builtin_ctor_named_arg_7455,
           keeps_a_record_construction_with_named_args_unchanged) {
    BEZEL_EXPECT_EQ(
        runAotOut(progWithRecord(
            "    var p := new P(x := 1, y := 2); "
            "    Console.WriteLine($\"{p.x} {p.y}\"); ")),
        std::string("1 2"));
}

// A BUILT-IN MEMBER call with named arguments — `Math.Max` — unchanged
// (the built-in member path is #7419's territory, not this lane's).
BEZEL_TEST(builtin_ctor_named_arg_7455,
           keeps_a_builtin_member_call_with_named_args_unchanged) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    Console.WriteLine(Math.Max(b := 2, a := 1)); ")),
        std::string("2"));
}

// ---------------------------------------------------------------------------
// The §22.5 REJECTIONS a built-in constructor shares with every other
// construction site — each must name §22.5.
// ---------------------------------------------------------------------------

// Naming a parameter ALREADY bound positionally is a compile error at the
// call naming that parameter (§22.5) — not an applicability failure.
BEZEL_TEST(builtin_ctor_named_arg_7455,
           rejects_a_builtin_constructor_parameter_bound_positionally_and_named) {
    JitResult r = runAot(prog(
        "    var dt := new DateTime(2024, year := 2025, month := 2); "));
    if (r.diagnostics.diagnostics().empty()) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kBoundTwiceDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSpec22_5));
}

// Naming the SAME parameter twice is likewise a compile error.
BEZEL_TEST(builtin_ctor_named_arg_7455,
           rejects_a_builtin_constructor_parameter_named_twice) {
    JitResult r = runAot(prog(
        "    var dt := new DateTime(year := 2024, year := 2025, month := 2); "));
    if (r.diagnostics.diagnostics().empty()) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kBoundTwiceDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSpec22_5));
}

// A POSITIONAL argument after a named one violates §22.5's ordering rule.
BEZEL_TEST(builtin_ctor_named_arg_7455,
           rejects_a_positional_argument_after_a_named_one_on_a_builtin_constructor) {
    JitResult r = runAot(prog(
        "    var dt := new DateTime(day := 1, 2, 3); "));
    if (r.diagnostics.diagnostics().empty()) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kPositionalAfterNamedDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSpec22_5));
}

// An unknown parameter name is a compile error (§22.5) — the names are
// checked, not discarded.
BEZEL_TEST(builtin_ctor_named_arg_7455,
           rejects_an_unknown_parameter_name_on_a_builtin_constructor) {
    JitResult r = runAot(prog(
        "    var dt := new DateTime(zzz := 1); "));
    if (r.diagnostics.diagnostics().empty()) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, "§22.5"));
}
