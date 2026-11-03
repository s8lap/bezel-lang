// Regression tests for the CLOSED-SURFACE's sibling family: a NULLABLE value
// that reaches a BUILT-IN parameter.
//
// The three issues pinned here share one mechanism family — "a nullable value
// reaches a built-in parameter that has no type opinion" — and each of them
// was MEASURED to be already fixed at this tip:
//
//   #6811 — a builtin non-string REFERENCE parameter declared non-nullable
//           accepted null (§7.3: null is not assignable to a non-nullable
//           target).  The refusal is §7's own wording.
//   #6823 — a §7.4-NARROWED nullable value passed to a built-in overload whose
//           parameter is not `string` reached the lowering as its
//           `{i8, T}` CARRIER instead of its payload: a silent wrong value or
//           an IR-verify abort.  §22.6: "a read of a binding §7.4 has narrowed
//           non-null is of its non-nullable type".
//   #7577 — the `builtin_admission_grid`'s cells `math_floor_nullable` and
//           `math_abs_decimal_nullable` EXPECTED a refusal that §7.4's
//           DECLARATION narrowing admits, so the grid called a correct
//           compiler wrong.  The correct expectations are the computed
//           values.
//
// SPEC (docs/Bezel_Language_Spec.md, v0.350).
//
// §7.3 (Nullability): null is not assignable to a non-nullable target.
// §7.4 (Narrowing): "the declaration's assignment" narrows a declared-nullable
//     local — "from a declaration whose initializer is such an expr" — and a
//     guarded or initializer-narrowed read is non-null.
// §22.6 (Overloading): "a read of a binding §7.4 has narrowed non-null is of
//     its non-nullable type".
//
// The refusal wording is §7's own: "the argument to `X` is nullable but the
// target type is non-nullable (§7)".
//
// EVERY ROW HERE IS A POSITIVE OBSERVABLE: a refusal asserts the EXACT
// diagnostic text, and a value asserts the EXACT stdout.  Nothing asserts
// the absence of a diagnostic, so no row is vacuous.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// ── #6811 — a builtin non-string REFERENCE parameter declared non-nullable ──
//
// `Console.WriteLine(value: Object)` (stdlib §2.1) and
// `InvalidOperationException(message: string, cause: Exception)` (spec §31.4)
// declare a reference parameter with NO `?`, so §7.3 refuses a null there.
const char* kConsoleNull =
    "the argument to `Console.WriteLine` is nullable but the target type is "
    "non-nullable (\xC2\xA7""7)";
const char* kExceptionNull =
    "the argument to `InvalidOperationException` constructor is nullable but "
    "the target type is non-nullable (\xC2\xA7""7)";

// ── #7577 — a DECLARATION-narrowed nullable at a built-in parameter ──
//
// §7.4 narrows a declared-nullable local whose initializer is a non-null
// expression, so `dn` is a `double` (not a `double?`) and §5.1 widens it to
// the parameter's `double`.  The grid's stale expectation was a REFUSAL; the
// correct expectation is the computed value.
const char* kFloorValue = "2\n";
const char* kAbsValue = "1.5\n";

std::string program(const std::string& body) {
    return "module M; public class P public static method Main(): int " + body +
           " return 0; end Main; end P;";
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// #6811 — the non-string REFERENCE parameter refuses null (§7.3)
// ─────────────────────────────────────────────────────────────────────────────

BEZEL_TEST(sema_builtin_nullable_param_6811,
           console_writeline_refuses_null_spec_7_3) {
    auto r = runAot(program("Console.WriteLine(null);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConsoleNull));
}

// The exception constructor's `cause: Exception` position (spec §31.4) is the
// other non-string reference position the issue names.
BEZEL_TEST(sema_builtin_nullable_param_6811,
           exception_cause_refuses_null_spec_7_3) {
    auto r = runAot(
        "module M; public class P public static method Main(): int "
        "  var e := new InvalidOperationException(\"m\", null); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kExceptionNull));
}

// The issue's own repro spelling is the NAMED form: `Console.WriteLine(value
// := null)`.  A named argument binds the declared parameter (§22.5), so §7's
// gate reads the BOUND position and refuses it.
BEZEL_TEST(sema_builtin_nullable_param_6811,
           console_writeline_named_null_refuses_null_spec_7_3) {
    auto r = runAot(program("Console.WriteLine(value := null);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConsoleNull));
}

// `Console.Write(value: Object)` (stdlib §2.1) is the sibling position.
BEZEL_TEST(sema_builtin_nullable_param_6811,
           console_write_refuses_null_spec_7_3) {
    auto r = runAot(program("Console.Write(null);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `Console.Write` is nullable but the target type is "
           "non-nullable (\xC2\xA7"
           "7)"));
}

// `TextWriter.Write(value: Object)` (stdlib §7.4) — reached through a
// `TextWriter`-typed local, the spelling the issue names.
BEZEL_TEST(sema_builtin_nullable_param_6811,
           textwriter_write_refuses_null_spec_7_3) {
    auto r = runAot(
        "module M; public class P public static method Main(): int "
        "  var w := Console.Out; "
        "  w.Write(null); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `TextWriter.Write` is nullable but the target type "
           "is non-nullable (\xC2\xA7"
           "7)"));
}

// The §31.4 `paramName: string` position of the two-argument exception
// constructors is non-nullable too (`ArgumentException`,
// `ArgumentOutOfRangeException`).
BEZEL_TEST(sema_builtin_nullable_param_6811,
           argument_exception_param_name_refuses_null_spec_7_3) {
    auto r = runAot(
        "module M; public class P public static method Main(): int "
        "  var e := new ArgumentOutOfRangeException(\"m\", null); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `ArgumentOutOfRangeException` constructor is "
           "nullable but the target type is non-nullable (\xC2\xA7"
           "7)"));
}

// ─────────────────────────────────────────────────────────────────────────────
// #6823 — a §7.4-NARROWED nullable reaches the lowering as its PAYLOAD
// (§22.6: "a read of a binding §7.4 has narrowed non-null is of its
// non-nullable type")
// ─────────────────────────────────────────────────────────────────────────────

// `StringBuilder.Append(value: int)` (stdlib §3.1) over a narrowed `int?`.
BEZEL_TEST(sema_builtin_nullable_param_6823,
           narrowed_int_append_lowers_the_payload_spec_22_6) {
    auto r = runAot(
        "module M; public class P "
        "  public static method Show(ni: int?): int "
        "    var sb := new StringBuilder(); "
        "    if ni <> null then sb.Append(ni); end; "
        "    Console.WriteLine(sb.ToString()); "
        "    return 0; end Show; "
        "  public static method Main(): int return Show(16); end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "16\n");
}

// `Int32.ToString(radix)` (stdlib §9.1) over a narrowed `int?` radix.
BEZEL_TEST(sema_builtin_nullable_param_6823,
           narrowed_int_tostring_radix_lowers_the_payload_spec_22_6) {
    auto r = runAot(
        "module M; public class P "
        "  public static method Show(ni: int?): int "
        "    var n := 255; "
        "    if ni <> null then Console.WriteLine(n.ToString(ni)); end; "
        "    return 0; end Show; "
        "  public static method Main(): int return Show(16); end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ff\n");
}

// The `double` twin of the `int` row — on the base this ABORTED the compiler
// in the LLVM verifier (`call ptr @bz_double_to_string_r({ i8, double } %16)`).
BEZEL_TEST(sema_builtin_nullable_param_6823,
           narrowed_double_append_lowers_the_payload_spec_22_6) {
    auto r = runAot(
        "module M; public class P "
        "  public static method Show(dn: double?): int "
        "    var sb := new StringBuilder(); "
        "    if dn <> null then sb.Append(dn); end; "
        "    Console.WriteLine(sb.ToString()); "
        "    return 0; end Show; "
        "  public static method Main(): int return Show(1.5); end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1.5\n");
}

// The `char` twin — on the base this ABORTED the compiler in the LLVM
// verifier (`call ptr @bz_char_to_string({ i8, i32 } %11)`).
BEZEL_TEST(sema_builtin_nullable_param_6823,
           narrowed_char_append_lowers_the_payload_spec_22_6) {
    auto r = runAot(
        "module M; public class P "
        "  public static method Show(ch: char?): int "
        "    var sb := new StringBuilder(); "
        "    if ch <> null then sb.Append(ch); end; "
        "    Console.WriteLine(sb.ToString()); "
        "    return 0; end Show; "
        "  public static method Main(): int return Show('A'); end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "A\n");
}

// `Int32.Parse(s: ReadOnlyStringSpan)` (stdlib §9.1) over a narrowed span.
BEZEL_TEST(sema_builtin_nullable_param_6823,
           narrowed_span_parse_lowers_the_payload_spec_22_6) {
    auto r = runAot(
        "module M; public class P public static method Main(): int "
        "  const src := \"42\"; "
        "  var sp: ReadOnlyStringSpan? := "
        "      (if src.Length > 0 then src.AsSpan(0) else null); "
        "  if sp <> null then Console.WriteLine(int.Parse(sp)); end; "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// #7577 — the DECLARATION-narrowed nullable at a built-in parameter
// computes (the grid's stale expectation)
// ─────────────────────────────────────────────────────────────────────────────

// `var dn: double? := 2.5;` — §7.4 narrows the DECLARATION, so `dn` is a
// `double` and `Math.Floor(dn)` computes `2`.
BEZEL_TEST(sema_builtin_nullable_param_7577,
           declaration_narrowed_double_floor_computes_spec_7_4) {
    auto r = runAot(
        "module M; public class P public static method Main(): int "
        "  var dn: double? := 2.5; "
        "  Console.WriteLine(Math.Floor(dn)); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, kFloorValue);
}

// The `decimal` twin — the grid's `math_abs_decimal_nullable` cell.
BEZEL_TEST(sema_builtin_nullable_param_7577,
           declaration_narrowed_decimal_abs_computes_spec_7_4) {
    auto r = runAot(
        "module M; public class P public static method Main(): int "
        "  var dn: decimal? := 1.5m; "
        "  Console.WriteLine(Math.Abs(dn)); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, kAbsValue);
}

// The `null`-initializer twin is NOT narrowed (§7.4 narrows only a
// non-null initializer), so it stays a refusal — the grid's
// `math_floor_nullable_null` cell is correct as it is, and this row pins it.
BEZEL_TEST(sema_builtin_nullable_param_7577,
           null_initializer_is_not_narrowed_spec_7_4) {
    auto r = runAot(
        "module M; public class P public static method Main(): int "
        "  var dn: double? := null; "
        "  Console.WriteLine(Math.Floor(dn)); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "Math.Floor"));
}
