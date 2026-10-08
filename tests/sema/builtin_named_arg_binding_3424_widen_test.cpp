// Spec-derived regression test for issues #3424, #3533, #3534 — widening the
// built-in parameter-name table and the constructor parameter-name table.
//
// spec §22.5 "Optional parameters and named arguments" (v0.315);
// spec §17.7 "The `new` operator" (v0.315);
// stdlib §2.2, §2.3, §2.5, §2.14, §2.15, §3.1, §7.2.
//
// WHAT THE DEFECT WAS.
// Issue #3424: `Math` and `Convert` static methods had no parameter-name rows,
// so named arguments on them were rejected with the limitation diagnostic
// rather than bound by the §22.5 binder.
// Issue #3533: `DateTime`'s eight-parameter constructor exceeded the row's
// four-name capacity, so named arguments on it were rejected.
// Issue #3534: `FileStream`, `JsonSerializer`, `JsonDeserializer`,
// `BinaryDeserializer` constructors had no rows, so named arguments on them
// were rejected.
//
// WHAT THE FIX DOES.
// Widened `BuiltinParamNameRow::names` from 4 to 8.
// Added `Math` and `Convert` parameter-name tables and their heads to
// `kBuiltinNamedArgTableHeads`.
// Added the `DateTime` 8-parameter constructor row.
// Added `FileStream`, `JsonSerializer`, `JsonDeserializer`,
// `BinaryDeserializer` constructor rows.
//
// NOTE ON PROOF HIERARCHY.
// - Math/Convert accept rows (e.g., Math.Abs, Convert.ToInt32) pass on BOTH pre-fix
//   and post-fix trees: the named-arg check is gated by `isBuiltinNamedArgTableHead`,
//   and Math/Convert were not in that gate pre-fix, so the check short-circuits.
//   These are compiler-regression sanity checks, not fix-proof rows.
// - DateTime 8-param ctor accept rows FAIL pre-fix (no row), PASS post-fix: these
//   ARE the primary fix-proof for #3533.
// - FileStream/JsonSerializer/JsonDeserializer/BinaryDeserializer constructor rows
//   FAIL pre-fix (no row), PASS post-fix: these ARE the fix-proof for #3534.
// - Math.DivRem and Convert.TryTo* have an `out` parameter: §22.5 makes the `out`
//   parameter positional-only, but the BY-VALUE parameters (a, b for DivRem; value
//   for TryTo*) ARE named in the table and may be used as named arguments in a call.
//   The table rows use a PARTIAL-NAMING shape: non-named slots are empty strings.
//   This enables naming by-value args without permitting naming the out param.
//
//   The two `rejects_out_param_*` rows below are discriminating guards: they
//   assert that naming the `out` parameter is rejected with the correct diagnostic
//   (post-fix). Pre-fix, these rows fail because the call is rejected with
//   "named arguments are not supported" (different diagnostic).
//   If a future change fills the empty slots with the out-param names, these
//   rows correctly flip to accept-rows, exposing the violation.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Serialization types (JsonSerializer, JsonDeserializer, BinaryDeserializer)
// require this import per stdlib §2.14/§2.15.
const char* kSerializationImport = "import System.Serialization; ";

std::string prog(const std::string& body) {
    // #5230: `@DATA@` splices to a per-process dir under wtScratchDir(). The
    // FileStream rows below name a data path, so a fixed one would both break
    // the /tmp scratch policy and race with a concurrent test process.
    return bezel::test::spliceDataPaths(
        "module M; "
        "class C "
        "  public static method Main(): int " +
            body +
            "    return 0; "
            "  end Main; "
            "end C;",
        "bzt3424");
}

std::string serProg(const std::string& body) {
    return "module M; " + std::string(kSerializationImport) +
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

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

}  // namespace

// ---------------------------------------------------------------------------
// fix #3424 — Math static methods (§2.2)
// ---------------------------------------------------------------------------

// fix #3424 / stdlib §2.2 / §22.5: Math.Abs(x: int) accepts named `x`.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_arg_on_math_abs) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Math.Abs(x := -42));")),
        std::string("42"));
}

// fix #3424 / stdlib §2.2 / §22.5: Math.Min(a, b) accepts named args.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_args_on_math_min) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Math.Min(a := 3, b := 7));")),
        std::string("3"));
}

// fix #3424 / stdlib §2.2 / §22.5: Math.Max(a, b) accepts named args.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_args_on_math_max) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Math.Max(a := 3, b := 7));")),
        std::string("7"));
}

// fix #3424 / stdlib §2.2 / §22.5: Math.Clamp(value, low, high) accepts named args.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_args_on_math_clamp) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Math.Clamp(value := 15, low := 0, high := 10));")),
        std::string("10"));
}

// fix #3424 / stdlib §2.2 / §22.5: Math.Pow(x, y) accepts named args.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_args_on_math_pow) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Math.Pow(x := 2.0, y := 8.0));")),
        std::string("256"));
}

// fix #3424 / stdlib §2.2 / §22.5: Math.Round(x, digits) — digits has default 0.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_args_on_math_round) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Math.Round(x := 3.14159, digits := 2));")),
        std::string("3.14"));
}

// fix #3424 / stdlib §2.2 / §22.5: Math.Sqrt(x) accepts named `x`.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_arg_on_math_sqrt) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Math.Sqrt(x := 81.0));")),
        std::string("9"));
}

// fix #3424 / stdlib §2.2 / §22.5: Math.DivRem(a, b, out remainder) — the
// partial-naming row names `a` and `b` but leaves the `out` slot empty.
// Naming `remainder` is rejected: both candidate overloads reject `remainder`
// as a named arg, so the binder fires "match none of the declared overloads".
// Pre-fix (no table row): rejected with "named arguments are not supported".
// This row guards the partial-naming shape: if the `out` slot is filled with
// "remainder", these reject-rows flip to accepts, exposing the violation.
BEZEL_TEST(builtin_named_arg_3424_widen,
           rejects_out_param_named_on_math_divrem) {
    JitResult r = runAot(prog(
        "    var q := Math.DivRem(a := 17, b := 5, remainder := 0); "
        "    Console.WriteLine(q); "));
    if (!hasError(r, "match none of the declared overloads")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, "match none of the declared overloads"));
}

// ---------------------------------------------------------------------------
// fix #3424 — Convert static methods (§2.3)
// ---------------------------------------------------------------------------

// fix #3424 / stdlib §2.3 / §22.5: Convert.ToInt32(value: string) accepts named `value`.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_arg_on_convert_toint32) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Convert.ToInt32(value := \"42\"));")),
        std::string("42"));
}

// fix #3424 / stdlib §2.3 / §22.5: Convert.ToDouble(value: string) accepts named `value`.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_arg_on_convert_todouble) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Convert.ToDouble(value := \"3.14\"));")),
        std::string("3.14"));
}

// fix #3424 / stdlib §2.3 / §22.5: Convert.ToString(value: int) accepts named `value`.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_arg_on_convert_tostring_int) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Convert.ToString(value := 42));")),
        std::string("42"));
}

// fix #3424 / stdlib §2.3 / §22.5: Convert.ToString(value: int, toBase: int) — both named.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_args_on_convert_tostring_base) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Convert.ToString(value := 255, toBase := 16));")),
        std::string("ff"));
}

// fix #3424 / stdlib §2.3 / §22.5: Convert.TryToInt32(value, out result) —
// naming `value` is accepted (partial-naming row); naming `result` is rejected.
// After the 1-param candidate is eliminated (it rejects `result`), the 2-param
// candidate is the sole remaining overload and fires "has no parameter named
// `result`" when the binder processes `result` against it.
// Pre-fix (no table row): rejected with "named arguments are not supported".
BEZEL_TEST(builtin_named_arg_3424_widen,
           rejects_out_param_named_on_convert_trytoint32) {
    JitResult r = runAot(prog(
        "    var ok := Convert.TryToInt32(value := \"42\", result := 0); "
        "    Console.WriteLine(ok); "));
    if (!hasError(r, "has no parameter named")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, "has no parameter named"));
    BEZEL_EXPECT_TRUE(hasError(r, "`result`"));
}

// ---------------------------------------------------------------------------
// fix #3533 — DateTime 8-parameter constructor (§2.5)
// ---------------------------------------------------------------------------

// fix #3533 / stdlib §2.5 / §22.5: DateTime(year, month, day, hour, minute,
// second, millisecond, kind) — all 8 named.  Uses the positional value as the
// reference (verified positional: 2024-03-15 01:02:03 => 638460613230000000).
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_args_on_datetime_8_param_ctor) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d := new DateTime(year := 2024, month := 3, day := 15, "
            "hour := 1, minute := 2, second := 3, millisecond := 0, kind := 0); "
            "    Console.WriteLine(d.Ticks); ")),
        std::string("638460613230000000"));
}

// fix #3533: partial named args (only the 6 required params named; millisecond
// and kind take defaults).
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_partial_named_args_on_datetime_8_param_ctor) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d := new DateTime(year := 2024, month := 3, day := 15, "
            "hour := 1, minute := 2, second := 3); "
            "    Console.WriteLine(d.Ticks); ")),
        std::string("638460613230000000"));
}

// fix #3533: wrong name on DateTime 8-param ctor is rejected.
BEZEL_TEST(builtin_named_arg_3424_widen,
           rejects_wrong_name_on_datetime_8_param_ctor) {
    JitResult r = runAot(prog(
        "    var d := new DateTime(year := 2024, month := 3, day := 15, "
        "hour := 1, minute := 2, second := 3, wrongName := 0); "
        "    Console.WriteLine(d.Ticks); "));
    if (!hasError(r, "has no parameter named")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, "has no parameter named"));
    BEZEL_EXPECT_TRUE(hasError(r, "`wrongName`"));
}

// ---------------------------------------------------------------------------
// fix #3534 — FileStream constructors (§7.2)
// ---------------------------------------------------------------------------
// NOTE: FileStream constructors are covered by the table, but the instance
// methods (Read, Write, etc.) are NOT. Full integration testing of the
// FileStream constructor requires imports that are complex to set up in this
// harness; the DateTime 8-param ctor test (above) provides the constructor
// named-argument coverage proof.
// FileStream(path, mode) — verifies the 2-param constructor compiles.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_args_on_filestream_2_param_ctor) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var f := new FileStream(path := \"@DATA@/bezel_test_fs.tmp\", "
            "mode := FileMode.Create); "
            "    Console.WriteLine(0); ")),
        std::string("0"));
}

// fix #3534 / stdlib §7.2 / §22.5: FileStream(path, mode, access) — all 3 named.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_named_args_on_filestream_3_param_ctor) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var f := new FileStream(path := \"@DATA@/bezel_test_fs2.tmp\", "
            "mode := FileMode.Create, access := FileAccess.ReadWrite); "
            "    Console.WriteLine(0); ")),
        std::string("0"));
}

// ---------------------------------------------------------------------------
// fix #3534 — JsonSerializer constructors (§2.15)
// ---------------------------------------------------------------------------
// fix #3534 / stdlib §2.15 / §22.5: JsonSerializer(options) — named `options`.
// The outer call uses a named arg on the JsonSerializer ctor; the inner
// JsonOptions() is POSITIONAL to avoid triggering JsonOptions' own ctor
// named-arg path (JsonOptions is not in the constructor named-arg table).
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_json_serializer_options_ctor_named_arg) {
    BEZEL_EXPECT_EQ(
        runAotOut(serProg(
            "    using var js := new JsonSerializer(options := new JsonOptions()); "
            "    Console.WriteLine(0); ")),
        std::string("0"));
}

// ---------------------------------------------------------------------------
// fix #3534 — JsonDeserializer constructors (§2.15)
// ---------------------------------------------------------------------------
// fix #3534 / stdlib §2.15 / §22.5: JsonDeserializer(input) — named `input`.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_json_deserializer_input_ctor_named_arg) {
    BEZEL_EXPECT_EQ(
        runAotOut(serProg(
            "    var json: Array<byte> := [78, 85, 76]; "
            "    using var jd := new JsonDeserializer(input := json.AsReadOnlySpan()); "
            "    Console.WriteLine(0); ")),
        std::string("0"));
}

// ---------------------------------------------------------------------------
// fix #3534 — BinaryDeserializer constructor (§2.14)
// ---------------------------------------------------------------------------
// fix #3534 / stdlib §2.14 / §22.5:
// BinaryDeserializer(input, ignoreUnknownFields) — both params named.
BEZEL_TEST(builtin_named_arg_3424_widen,
           accepts_binary_deserializer_2_param_ctor_named_args) {
    BEZEL_EXPECT_EQ(
        runAotOut(serProg(
            "    var data := new Array<byte>(0); "
            "    using var bd := new BinaryDeserializer(input := data.AsReadOnlySpan(), "
            "ignoreUnknownFields := false); "
            "    Console.WriteLine(0); ")),
        std::string("0"));
}
