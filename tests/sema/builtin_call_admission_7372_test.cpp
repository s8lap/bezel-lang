// campaign 2 (`builtin-call-admission`), the ANCHOR: the BUILTIN CALL
// ADMISSION GATE.
//
// §22.6 (v0.344): "If no candidate is applicable at all — no candidate is
// reachable: a conversion §5.1 does not admit as implicit, a parameter-count
// difference at a function-typed slot, a method-reference formation that
// fails, or an inference that finds no binding — the call is a compile error
// with no candidates cited, because no overload was reachable."
//
// stdlib §9.1 (v0.344) declares `Parse(s: string)`, `Parse(s: string, radix:
// int)`, `TryParse(s: string, out result: int)` and
// `TryParse(s: string, radix: int, out result: int)`; §9.2-§9.5 declare the
// same shape for the floating, decimal, boolean and char types; §2.6 declares
// `Guid.Parse(s: string)` / `Guid.TryParse(s: string, out result: Guid)`.
//
// The FIRST argument of every one of those forms is a `string` (or a
// `ReadOnlyStringSpan`), and §5.1 admits NO conversion to `string` from an
// `int`, a `bool`, a `char`, a `double` or an `Object`. So a call that
// reaches one of these with a non-string first argument has NO reachable
// candidate and is a compile error.
//
// The R35 rulings for #7290 / #7293 / #7294 chose "guard-and-drop" for the
// parse-operand guards: the call was SILENTLY DROPPED with an `internal:`
// degrade, so the program compiled and answered `false` (or emitted nothing)
// for a program the standard library makes a compile error. Issue #7372
// tracks that shape; this file pins the ADMISSION half.
//
// THE REJECTION ORACLE is the diagnostic text plus a real location
// (`failedAtCompileTimeWithRealLocation`), never the absence of a diagnostic.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// The rejection oracle: the named diagnostic, at a real location, and the
// compiler did not merely abort.
void expectRefused(const JitResult& r, const std::string& needle) {
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// The program the row runs. `body` is the statement(s); `pre` the locals.
std::string prog(const std::string& pre, const std::string& body) {
    return R"(
module M;
public class Program
  public static method Main(): int
)" + pre + R"(
)" + body + R"(
    return 0;
  end Main;
end Program;
)";
}

}  // namespace

// --- the Parse family: a non-string first argument has no reachable candidate ---

// stdlib §9.1 declares `Parse(s: string): int`. An `int` reaches no §9.1
// parameter (§5.1 admits no conversion to `string`), so §22.6 makes the call
// a compile error. Before the anchor it compiled and printed `0` with an
// `internal:` degrade — a silent acceptance inversion.
BEZEL_TEST(builtin_call_admission_7372, int_parse_with_an_int_argument_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(16));"));
    expectRefused(r, "no matching overload of `int.Parse`");
}

// The same shape with a `bool`, a `char` and a `double` first argument.
BEZEL_TEST(builtin_call_admission_7372, int_parse_with_a_bool_argument_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(true));"));
    expectRefused(r, "no matching overload of `int.Parse`");
}

BEZEL_TEST(builtin_call_admission_7372, int_parse_with_a_char_argument_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse('a'));"));
    expectRefused(r, "no matching overload of `int.Parse`");
}

BEZEL_TEST(builtin_call_admission_7372, int_parse_with_a_double_argument_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(1.5));"));
    expectRefused(r, "no matching overload of `int.Parse`");
}

// A CLASS INSTANCE reaches no §9.1 parameter either — and before the anchor
// the compiler lowered the object's POINTER as a string and read FOREIGN
// MEMORY as text (issue #7294's measurement).
BEZEL_TEST(builtin_call_admission_7372, int_parse_with_an_object_argument_is_refused) {
    auto r = runAot(prog("    var o: Object := 1;",
                         "    Console.WriteLine(int.Parse(o));"));
    expectRefused(r, "no matching overload of `int.Parse`");
}

// The `TryParse` family: `TryParse(s: string, out result: int)` — the first
// argument is still a `string`.
BEZEL_TEST(builtin_call_admission_7372, int_tryparse_with_an_int_argument_is_refused) {
    auto r = runAot(prog("    var v: int := 0;",
                         "    _ := int.TryParse(16, out v);"));
    expectRefused(r, "no matching overload of `int.TryParse`");
}

BEZEL_TEST(builtin_call_admission_7372, int_tryparse_with_a_double_argument_is_refused) {
    auto r = runAot(prog("    var v: int := 0;",
                         "    _ := int.TryParse(1.5, out v);"));
    expectRefused(r, "no matching overload of `int.TryParse`");
}

// stdlib §9.1's OTHER integer receivers: `long.Parse(s: string): long` and
// `long.TryParse(s: string, out result: long): bool` declare the same shape,
// so the same admission question reaches them. (The review's coverage gap:
// `long.Parse(16)` was refused by the tip and pinned by NO row.)
BEZEL_TEST(builtin_call_admission_7372, long_parse_with_an_int_argument_is_refused) {
    auto r = runAot(prog("", "    _ := long.Parse(16);"));
    expectRefused(r, "no matching overload of `long.Parse`");
}

BEZEL_TEST(builtin_call_admission_7372, long_tryparse_with_an_int_argument_is_refused) {
    auto r = runAot(prog("    var v: long := 0;",
                         "    _ := long.TryParse(16, out v);"));
    expectRefused(r, "no matching overload of `long.TryParse`");
}

// §7.3: the `null` literal reaches no non-nullable `string` parameter. The
// base arm ACCEPTED `long.Parse(null)` and `long.TryParse(null, out v)`
// (printing `0`), which is what issue #6825's report is about: the #6782
// type map had no `Int64` head, so the null check never reached the other
// integer receivers.
BEZEL_TEST(builtin_call_admission_7372, long_parse_with_null_is_refused) {
    auto r = runAot(prog("", "    _ := long.Parse(null);"));
    expectRefused(r, "no matching overload of `long.Parse`");
}

BEZEL_TEST(builtin_call_admission_7372, long_tryparse_with_null_is_refused) {
    auto r = runAot(prog("    var v: long := 0;",
                         "    _ := long.TryParse(null, out v);"));
    expectRefused(r, "no matching overload of `long.TryParse`");
}

// The other members of the family: bool (§9.4), char (§9.5), double (§9.2)
// and Guid (§2.6).
BEZEL_TEST(builtin_call_admission_7372, bool_tryparse_with_an_int_argument_is_refused) {
    auto r = runAot(prog("    var v: bool := false;",
                         "    _ := bool.TryParse(1, out v);"));
    expectRefused(r, "no matching overload of `bool.TryParse`");
}

BEZEL_TEST(builtin_call_admission_7372, char_tryparse_with_an_int_argument_is_refused) {
    auto r = runAot(prog("    var v: char := 'x';",
                         "    _ := char.TryParse(1, out v);"));
    expectRefused(r, "no matching overload of `char.TryParse`");
}

BEZEL_TEST(builtin_call_admission_7372, double_tryparse_with_an_int_argument_is_refused) {
    auto r = runAot(prog("    var v: double := 0.0;",
                         "    _ := double.TryParse(1, out v);"));
    expectRefused(r, "no matching overload of `double.TryParse`");
}

BEZEL_TEST(builtin_call_admission_7372, guid_tryparse_with_an_int_argument_is_refused) {
    auto r = runAot(prog("    var g: Guid := Guid.Empty;",
                         "    _ := Guid.TryParse(1, out g);"));
    expectRefused(r, "no matching overload of `Guid.TryParse`");
}

// stdlib §9.3 declares `Decimal.Parse(s: string): decimal`.
BEZEL_TEST(builtin_call_admission_7372, decimal_parse_with_a_double_argument_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(decimal.Parse(1.5));"));
    expectRefused(r, "no matching overload of `decimal.Parse`");
}

// --- the ARITY question: a form the standard library does not declare ---

// stdlib §9.5 declares `Char.Parse(s: string): char` and NO radix form, so
// `char.Parse("a", 16)` reaches no candidate at arity 2.
BEZEL_TEST(builtin_call_admission_7372, char_parse_with_a_radix_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(char.Parse(\"a\", 16));"));
    expectRefused(r, "no matching overload of `char.Parse`");
}

// stdlib §9.3 declares `Decimal.ToString(): string` and
// `Decimal.ToString(format: string): string` — and NO radix form, so
// `(15m).ToString(16)` reaches no candidate. Before the anchor it ABORTED the
// compiler.
BEZEL_TEST(builtin_call_admission_7372, decimal_tostring_with_a_radix_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine((15m).ToString(16));"));
    expectRefused(r, "no matching overload of `decimal.ToString`");
}

// --- B6: a null at a built-in parameter declared non-nullable (§7.3) ---

// stdlib §2.4 declares `Environment.GetInt(name: string): int?` — the
// parameter is non-nullable, so the `null` literal reaches no candidate
// (§7.3). Before the anchor this compiled with an `internal:` degrade.
BEZEL_TEST(builtin_call_admission_7372, environment_getint_with_null_is_refused) {
    auto r = runAot(prog("", "    var v := Environment.GetInt(null);"));
    expectRefused(r, "nullable but the target type is non-nullable");
}

// stdlib §7.3 declares `File.ReadAllTextAsync(path: string, token:
// CancellationToken := ...)`.
BEZEL_TEST(builtin_call_admission_7372, file_readalltextasync_with_null_is_refused) {
    auto r = runAot(prog("", "    var t := File.ReadAllTextAsync(null);"));
    expectRefused(r, "nullable but the target type is non-nullable");
}

// stdlib §2.4 declares `Get(name: string): string?` — the parameter is
// non-nullable, so the `null` literal reaches no candidate (§7.3). Before the
// anchor this compiled and ran.
BEZEL_TEST(builtin_call_admission_7372, environment_get_with_null_is_refused) {
    auto r = runAot(prog("", "    var v := Environment.Get(null);"));
    expectRefused(r, "nullable but the target type is non-nullable");
}

// stdlib §2.4 declares `Set(name: string, value: string?)` — the FIRST
// parameter is non-nullable and the SECOND is declared `string?`, so only the
// first is a §7.3 refusal.
BEZEL_TEST(builtin_call_admission_7372, environment_set_with_null_is_refused) {
    auto r = runAot(prog("", "    Environment.Set(null, \"v\");"));
    expectRefused(r, "nullable but the target type is non-nullable");
}

// stdlib §10.1 declares `CString.From(s: string): owned CString`.
BEZEL_TEST(builtin_call_admission_7372, cstring_from_with_null_is_refused) {
    auto r = runAot(prog("", "    var c := CString.From(null);"));
    expectRefused(r, "nullable but the target type is non-nullable");
}

// stdlib §10.2 declares `WString.From(s: string): owned WString`.
BEZEL_TEST(builtin_call_admission_7372, wstring_from_with_null_is_refused) {
    auto r = runAot(prog("", "    var w := WString.From(null);"));
    expectRefused(r, "nullable but the target type is non-nullable");
}

// --- the CONTROLS: the shapes the standard library ADMITS must not move ---

// stdlib §2.4's `Set(name: string, value: string?)` — the VALUE parameter is
// declared `string?`, so a `null` there is a legal argument. The non-nullable
// position must not spread to the nullable one.
BEZEL_TEST(builtin_call_admission_7372, environment_set_with_a_null_value_is_unchanged) {
    auto r = runAot(prog("", "    Environment.Set(\"BEZEL_ADMISSION_CONTROL\", null);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}


// --- the CONTROLS: the shapes the standard library ADMITS must not move ---

// §9.1's `Parse(s: string): int`.
BEZEL_TEST(builtin_call_admission_7372, int_parse_with_a_string_argument_is_unchanged) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(\"16\"));"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "16\n");
}

// §9.1's `TryParse(s: string, out result: int): bool`.
BEZEL_TEST(builtin_call_admission_7372, int_tryparse_with_a_string_argument_is_unchanged) {
    auto r = runAot(prog("    var v: int := 0;",
                         "    Console.WriteLine(int.TryParse(\"16\", out v));\n"
                         "    Console.WriteLine(v);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n16\n");
}

// §9.1's `Parse(s: string, radix: int): int`.
BEZEL_TEST(builtin_call_admission_7372, int_parse_with_a_radix_is_unchanged) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(\"ff\", 16));"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "255\n");
}

// §5.1's CONSTANT NARROWING: "the source may be a compile-time constant of
// any integer type — not only `int` — and the target any integer type whose
// range contains its value, across signedness". A `uint`-typed CONSTANT radix
// therefore REACHES the `int` parameter and the call is ADMITTED. This row is
// the anti-over-rejection control for the radix position, and it is also the
// row the landed #7285 fix names ("uint/ulong/long const radix still parses
// hex").
BEZEL_TEST(builtin_call_admission_7372, int_parse_with_a_uint_constant_radix_is_admitted) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(\"ff\", 16u));"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "255\n");
}

// §7.4's INITIALIZER NARROWING: "from a declaration whose initializer is
// such an expr — `var x: T? := expr;`". A local so declared is narrowed
// non-null, so the `string?` it holds REACHES the `string` parameter and the
// call is ADMITTED. This row is the anti-over-rejection control for the
// nullability position: the gate must not refuse a narrowed read.
BEZEL_TEST(builtin_call_admission_7372, int_tryparse_with_a_narrowed_local_is_admitted) {
    auto r = runAot(prog("    var v: int := 0;\n    var s: string? := \"16\";",
                         "    _ := int.TryParse(s, out v);\n"
                         "    Console.WriteLine(v);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "16\n");
}

// §9.2 / §9.3 / §9.4 / §9.5 / §2.6: the other members' string forms.
BEZEL_TEST(builtin_call_admission_7372, the_other_parse_families_are_unchanged) {
    auto r = runAot(prog("", "    Console.WriteLine(double.Parse(\"1.5\"));\n"
                            "    Console.WriteLine(decimal.Parse(\"1.5\"));\n"
                            "    Console.WriteLine(bool.Parse(\"true\"));\n"
                            "    Console.WriteLine(char.Parse(\"a\"));"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1.5\n1.5\nTrue\na\n");
}

// §9.1's `ToString(radix: int): string` and §9.3's
// `ToString(format: string): string`.
BEZEL_TEST(builtin_call_admission_7372, the_tostring_families_are_unchanged) {
    auto r = runAot(prog("", "    Console.WriteLine((255).ToString(16));\n"
                            "    Console.WriteLine((1.5m).ToString(\"F2\"));"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "ff\n1.50\n");
}

// stdlib §2.18 (v0.345): `bool` and `char` are intrinsically formattable with
// an EMPTY vocabulary — the `ToString(format: string)` FORM is declared for
// them and a format SPEC on them is a compile error. So a RUNTIME format
// string on a `bool` / `char` receiver reaches the declared form and is
// ADMITTED (the spec's emptiness is the format-spec gate's business, not
// the admission gate's). These two rows are the anti-over-rejection controls
// for the `ToString(format: string)` rows of `Boolean` and `Char`.
BEZEL_TEST(builtin_call_admission_7372,
           bool_to_string_with_a_runtime_format_is_unchanged) {
    auto r = runAot(prog("", "    var b := true;\n"
                            "    var fmt: string := \"F4\";\n"
                            "    var s := b.ToString(fmt);\n"
                            "    Console.WriteLine(s);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}

BEZEL_TEST(builtin_call_admission_7372,
           char_to_string_with_a_runtime_format_is_unchanged) {
    auto r = runAot(prog("", "    var c := 'a';\n"
                            "    var fmt: string := \"F4\";\n"
                            "    var s := c.ToString(fmt);\n"
                            "    Console.WriteLine(s);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}

// §7.4's NULL-TEST NARROWING at a RECORD-typed parameter: `if sp <> null
// then …` narrows the `ReadOnlyStringSpan?` local, and the narrowed read
// REACHES the `Parse(s: ReadOnlyStringSpan)` overload (stdlib §9.1). This is
// the anti-over-rejection control for the narrowing read: the gate must try
// EVERY declared form of the member (the `string` form AND the span form),
// and it must read the narrowed type, not the declared `T?`.
BEZEL_TEST(builtin_call_admission_7372,
           int_parse_with_a_narrowed_span_is_unchanged) {
    auto r = runAot(prog("    const src := \"42\";\n"
                         "    var sp: ReadOnlyStringSpan? := (if src.Length > 0 "
                         "then src.AsSpan(0) else null);",
                         "    if sp <> null then var n := int.Parse(sp); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}
