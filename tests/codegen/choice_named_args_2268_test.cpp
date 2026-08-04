// Codegen tests for issue #2268 — §35.3 / §22.5 (v0.236): named arguments on
// a CHOICE CASE construction (`Choice.Case(name := expr)`) must bind by
// payload field NAME, not by source position.
//
// The fix (LlvmBackend_Calls2.cpp:3287-3330, replacing the legacy
// "§A.6 CallExpr's `args` carry positional arguments only" comment that
// indexed payload fields positionally): the codegen now builds a
// payloadArg[] lookup keyed on the case's payload declaration order and
// fills it from named or positional args, mirroring the record path at
// LlvmBackend_Exprs2.cpp:5316-5348 (#540) and the §22.5 binding rule
// sema performs (bindArgsToParams, ExprTypecheck_Calls.cpp:7996).
//
// Each test pins a distinct way the positional bug mis-binds. The two-field
// scrambled case is the canonical regression: pre-fix `Opt.Two(y := 7,
// x := 5)` stored 7 into field `x` and 5 into field `y` — observable as
// the wrong value (75) on read-back. Post-fix the same call returns the
// correct 57. Each ACCEPT case returns a distinct exit code so a silently
// wrong value is caught too.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Two-field value-like choice — every payload is value-typed, so the
// §35.4 classifier returns ValueLike and no ownership/disposal rule rejects
// the program (mirrors choice_codegen_35_test.cpp's value-like-only stance).
const char* kCommon =
    "module M; "
    "choice Opt Two(x: int, y: int), None; end Opt; "
    "class App public static method Main(): int ";

int run(const std::string& tail) {
    auto r = runAot(std::string(kCommon) + tail + " end Main; end App;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ===========================================================================
// REGRESSION — §35.3 / §22.5 / #2268: named args in REVERSED declaration
// order must produce the same record as positional args. Pre-fix this
// silently stored `7` into field `x` and `5` into field `y` (the bug), so
// `x*10 + y` read back as `75`. Post-fix the field binding follows the
// name, not the source index: x=5, y=7, total=57.
//
// This is the primary regression case the issue calls out.
// ===========================================================================
BEZEL_TEST(codegen_choice_named_args_2268, reversed_order_two_fields) {
    BEZEL_EXPECT_EQ(run(
        "  var o := Opt.Two(y := 7, x := 5); "
        "  case o of "
        "    Two(var a, var b) => do return a * 10 + b; end, "
        "    None => do return -1; end "
        "  end; "), 57);
}

// §35.3 / §22.5 / #2268: named args in declaration order — the trivial case
// (no reorder) — must also bind by name, not by source index.
BEZEL_TEST(codegen_choice_named_args_2268, declaration_order_two_fields) {
    BEZEL_EXPECT_EQ(run(
        "  var o := Opt.Two(x := 3, y := 4); "
        "  case o of "
        "    Two(var a, var b) => do return a * 10 + b; end, "
        "    None => do return -1; end "
        "  end; "), 34);
}

// §35.3 / §22.5 / #2268: three payload fields, scrambled source order.
// `Opt.Three(c := 3, a := 1, b := 2)` must yield a=1, b=2, c=3 (total 123).
// Pre-fix the positional walker bound source index 0 → field `a`, so `3`
// landed in `a`, `1` in `b`, `2` in `c` — observable as `312`.
BEZEL_TEST(codegen_choice_named_args_2268, scrambled_three_fields) {
    auto r = runAot(
        "module M; "
        "choice Opt Three(a: int, b: int, c: int), None; end Opt; "
        "class App public static method Main(): int "
        "  var o := Opt.Three(c := 3, a := 1, b := 2); "
        "  case o of "
        "    Three(var a, var b, var c) => do return a * 100 + b * 10 + c; end, "
        "    None => do return -1; end "
        "  end; "
        "end Main; end App;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 123);
}

// CONTROL — §35.3 / §22.5 / #2268: POSITIONAL args on a choice case
// construction still bind by source index (this is the existing rule and
// the all-positional leg of the new code path). Guards the new
// allPositional branch hasn't been broken by the named-arg refactor.
BEZEL_TEST(codegen_choice_named_args_2268, positional_two_fields_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "  var o := Opt.Two(5, 7); "
        "  case o of "
        "    Two(var a, var b) => do return a * 10 + b; end, "
        "    None => do return -1; end "
        "  end; "), 57);
}

// CONTROL — §35.3 / §22.5 / #2268: a choice with a payload-FREE case still
// resolves. `Opt.None` (no parens) is the bare-MemberAccess path; sema
// rejects `Opt.None()` with §35.3 "payload case requires payload" or
// similar — irrelevant to the codegen path under test, so we only verify
// the payload-free path stays clean.
BEZEL_TEST(codegen_choice_named_args_2268, payload_free_case_still_works) {
    BEZEL_EXPECT_EQ(run(
        "  var o := Opt.None; "
        "  case o of "
        "    Two(var a, var b) => do return -1; end, "
        "    None => do return 99; end "
        "  end; "), 99);
}
