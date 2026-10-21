// Spec-derived regression test for issue #6782.
//
// §7 (Nullability, "parameter pass") and §7.3 (Assigning `null` to a
// non-nullable target), v0.340: a `null` argument, or an un-narrowed `string?`,
// passed at a parameter the standard library declares as a non-nullable
// `string` is a compile error. The user-method path enforces this through
// checkCallArity. A built-in member, static or constructor has no MethodSymbol,
// so that path never ran for it: `TimeSpan.ToString(null)` and
// `DateTime.ToString(null)` compiled, and the run either faulted or printed
// nothing. The declarations the rejects derive from are the stdlib entries
// `ToString(format: string)` (§2.5, §2.6, §9.1), `Parse(s: string)` (§2.5,
// §2.6, §9.1, §9.4), `StringBuilder(initial: string)` and `Replace(oldValue:
// string, newValue: string)` (§3.1), `File.Exists(path: string)` (§7.3),
// `Path.Combine(a: string, b: string)` (§7.4), `Convert.ToInt32(value: string)`
// (§2.3), and `constructor(message: string)` on every standard exception
// (§31.4).
//
// Each reject row asserts the exact diagnostic text AND the 1-based line and
// column of the argument, which is where the §7 report is made. The accept rows are the controls the fix must not reject: the
// parameterless and format-free forms, a narrowed `string?` (§7.4), the
// nullable-declared `Equals(other: string?)` (§11.2) and
// `String.IsNullOrWhiteSpace(value: string?)` (stdlib §9.6), a non-string
// `StringBuilder(capacity: int)`, and the narrowed nullables of the review
// (below) that reach a non-string overload or are `readonly` fields. Every
// accept row asserts that NO error is reported.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// A one-method program whose body is `body`, followed by `return 0;`.
std::string program(const char* body) {
    return std::string("module M; class C public static method T(): int ") +
           body + " return 0; end T; end C;";
}

// The same shape with a `string?` parameter `s` in scope. A `var` initialized
// from a non-null value is narrowed by its initializer (§7.4), so an
// un-narrowed `string?` operand has to be a parameter.
std::string withNullableParam(const char* body) {
    return std::string(
               "module M; class C public static method Show(s: string?): int ") +
           body + " return 0; end Show; end C;";
}

// True iff the program rejects with EXACTLY `message`, reported at the first
// occurrence of `needle` in the source (the argument's begin position).
bool rejectsAt(const std::string& src, std::string_view needle,
               const char* message) {
    const std::size_t at = src.find(needle);
    if (at == std::string::npos) return false;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    for (std::size_t i = 0; i < at; ++i) {
        if (src[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    auto r = parseSemaAll(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message == message && d.location.line == line &&
            d.location.column == column)
            return true;
    return false;
}

// True iff the program produces NO error at all. A false rejection of an
// accept row need not carry the §7 "nullable" text: the first cut of #6782
// reported a §5.1 "not implicitly convertible" error at a narrowed argument of
// a non-string overload, which a "nullable"-text oracle could not see.
bool acceptsWithoutErrors(const std::string& src) {
    auto r = parseSemaAll(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) return false;
    return true;
}

}  // namespace

// --- Reject rows: the measured families (#6782) --------------------------

// §2.5 / stdlib: `TimeSpan.ToString(format: string)`. The issue's own repro.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           timespan_tostring_null_rejected_s2_5) {
    const std::string src = program(
        "var a := TimeSpan.FromSeconds(5.0); var s := a.ToString(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `TimeSpan.ToString` is nullable but the target type "
        "is non-nullable (§7)"));
}

// §2.5 / stdlib: `DateTime.Parse(s: string)`, the static family.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           datetime_parse_null_rejected_s2_5) {
    const std::string src = program("var d := DateTime.Parse(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `DateTime.Parse` is nullable but the target type is "
        "non-nullable (§7)"));
}

// §2.5 / stdlib: `DateTime.ToString(format: string)`, the issue's second form.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           datetime_tostring_null_rejected_s2_5) {
    const std::string src = program(
        "var d := DateTime.Parse(\"2024-03-15\"); var s := d.ToString(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `DateTime.ToString` is nullable but the target type "
        "is non-nullable (§7)"));
}

// §2.6 / stdlib: `Guid.ParseOrNull(s: string)`.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           guid_parseornull_null_rejected_s2_6) {
    const std::string src = program("var g := Guid.ParseOrNull(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `Guid.ParseOrNull` is nullable but the target type is "
        "non-nullable (§7)"));
}

// §9.1 / stdlib: `Int32.ToString(format: string)` on an `int` receiver, and
// the keyword-spelled static `int.Parse(s: string)`.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           int_tostring_null_rejected_s9_1) {
    const std::string src = program("var n := 5; var s := n.ToString(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `int.ToString` is nullable but the target type is "
        "non-nullable (§7)"));
}

BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           int_parse_null_rejected_s9_1) {
    const std::string src = program("var n := int.Parse(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `int.Parse` is nullable but the target type is "
        "non-nullable (§7)"));
}

// §9.3 / stdlib: `decimal.ToString(format: string)`.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           decimal_tostring_null_rejected_s9_3) {
    const std::string src = program(
        "var m: decimal := 1.5; var s := m.ToString(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `decimal.ToString` is nullable but the target type is "
        "non-nullable (§7)"));
}

// §9.4 / stdlib: `Boolean.Parse(s: string)`, spelled with the keyword.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           bool_parse_null_rejected_s9_4) {
    const std::string src = program("var b := bool.Parse(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `bool.Parse` is nullable but the target type is "
        "non-nullable (§7)"));
}

// §9.6 / stdlib: the primitive `string` char|string overload sets, which
// `checkBuiltinStringCallArgs` now reads a null literal against.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           string_contains_null_rejected_s9_6) {
    const std::string src = program("var b := \"abc\".Contains(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `string.Contains` is nullable but the target type is "
        "non-nullable (§7)"));
}

BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           string_replace_second_null_rejected_s9_6) {
    const std::string src = program("var r := \"abc\".Replace(\"a\", null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `string.Replace` is nullable but the target type is "
        "non-nullable (§7)"));
}

BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           string_split_null_rejected_s9_6) {
    const std::string src = program("var a := \"a,b\".Split(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `string.Split` is nullable but the target type is "
        "non-nullable (§7)"));
}

// §3.1 / stdlib: `StringBuilder(initial: string)` and its instance methods.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           stringbuilder_ctor_null_rejected_s3_1) {
    const std::string src = program("var sb := new StringBuilder(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `StringBuilder` constructor is nullable but the "
        "target type is non-nullable (§7)"));
}

BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           stringbuilder_replace_first_null_rejected_s3_1) {
    const std::string src = program(
        "var sb := new StringBuilder(); sb.Replace(null, \"x\");");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null,",
        "the argument to `StringBuilder.Replace` is nullable but the target "
        "type is non-nullable (§7)"));
}

BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           stringbuilder_appendline_null_rejected_s3_1) {
    const std::string src = program(
        "var sb := new StringBuilder(); sb.AppendLine(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `StringBuilder.AppendLine` is nullable but the target "
        "type is non-nullable (§7)"));
}

// §31.4 / stdlib: `constructor(message: string)` on a standard exception, and
// the two-argument `ArgumentOutOfRangeException(message: string, paramName:
// string)`.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           keynotfound_ctor_null_rejected_s31_4) {
    const std::string src = program("var e := new KeyNotFoundException(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `KeyNotFoundException` constructor is nullable but "
        "the target type is non-nullable (§7)"));
}

BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           argoutofrange_pair_null_rejected_s31_4) {
    const std::string src = program(
        "var e := new ArgumentOutOfRangeException(\"m\", null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `ArgumentOutOfRangeException` constructor is nullable "
        "but the target type is non-nullable (§7)"));
}

// §31.4: `ArgumentException(message: string, paramName: string)`. Position 0 is
// the `message` of the two-argument form, which the position-0 rule covers.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           argexception_pair_first_null_rejected_s31_4) {
    const std::string src = program(
        "var e := new ArgumentException(null, \"p\");");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null,",
        "the argument to `ArgumentException` constructor is nullable but the "
        "target type is non-nullable (§7)"));
}

// §31.4: position 1 of the same pair is the `paramName` row.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           argexception_pair_second_null_rejected_s31_4) {
    const std::string src = program(
        "var e := new ArgumentException(\"m\", null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `ArgumentException` constructor is nullable but the "
        "target type is non-nullable (§7)"));
}

// §31.4: a type that declares no constructor carries `Exception`'s three,
// including `(message: string, cause: Exception)`, so its message is
// non-nullable as well. `InvalidOperationException` declares none.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           exception_message_cause_first_null_rejected_s31_4) {
    const std::string src = program(
        "var c := new Exception(\"c\"); "
        "var e := new InvalidOperationException(null, c);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null,",
        "the argument to `InvalidOperationException` constructor is nullable "
        "but the target type is non-nullable (§7)"));
}

// §31.4: the non-null two-argument forms are the accept controls.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           exception_pair_nonnull_accepted_s31_4) {
    const std::string src = program(
        "var c := new Exception(\"c\"); "
        "var e := new ArgumentException(\"m\", \"p\"); "
        "var f := new InvalidOperationException(\"m\", c);");
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(src));
}

// §7.3 / stdlib: `File.Exists(path: string)`.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           file_exists_null_rejected_s7_3) {
    const std::string src = program("var b := File.Exists(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `File.Exists` is nullable but the target type is "
        "non-nullable (§7)"));
}

// §7.4 / stdlib: `Path.Combine(a: string, b: string)`, the first argument.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           path_combine_first_null_rejected_s7_4) {
    const std::string src = program("var p := Path.Combine(null, \"x\");");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null,",
        "the argument to `Path.Combine` is nullable but the target type is "
        "non-nullable (§7)"));
}

// §2.3 / stdlib: `Convert.ToInt32(value: string)`.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           convert_toint32_null_rejected_s2_3) {
    const std::string src = program("var n := Convert.ToInt32(null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `Convert.ToInt32` is nullable but the target type is "
        "non-nullable (§7)"));
}

// §7 / §7.4: an UN-narrowed `string?` is rejected exactly as the `null` literal
// is, so the rule is nullability, not the literal. The operand is a parameter
// because an initializer narrows a `var` (§7.4). The needle `s);` starts at the
// argument, which is where checkAssignable reports.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           timespan_tostring_unnarrowed_nullable_rejected_s7_4) {
    const std::string src = withNullableParam(
        "var a := TimeSpan.FromSeconds(1.0); var t := a.ToString(s);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "s);",
        "the argument to `TimeSpan.ToString` is nullable but the target type "
        "is non-nullable (§7)"));
}

// --- Accept rows: the controls the fix must not reject --------------------

// §2.5 / stdlib: the parameterless and the `"c"` format forms are the
// non-nullable overload and the string literal, respectively.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           timespan_tostring_parameterless_and_format_accepted_s2_5) {
    const std::string src = program(
        "var a := TimeSpan.FromSeconds(5.0); var s := a.ToString(); "
        "var c := a.ToString(\"c\");");
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(src));
}

// §7.4 / §27.6: a NARROWED `string?` parameter is a `string` after the null
// guard, so it passes the non-nullable `format` parameter. The guard is what
// narrows it here; the un-guarded form is the reject row above.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           timespan_tostring_narrowed_nullable_accepted_s7_4) {
    const std::string src = withNullableParam(
        "if s = null then return 1; end; "
        "var a := TimeSpan.FromSeconds(1.0); var t := a.ToString(s);");
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(src));
}

// §11.2 (parameter-nullability widening): `Equals(other: string?)` takes null.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           string_equals_null_accepted_s11_2) {
    const std::string src = program("var b := \"abc\".Equals(null);");
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(src));
}

// stdlib §9.6: `IsNullOrWhiteSpace(value: string?)` is declared nullable.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           string_isnullorwhitespace_null_accepted_s9_6) {
    const std::string src = program("var b := String.IsNullOrWhiteSpace(null);");
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(src));
}

// §3.1: `StringBuilder(capacity: int)` takes an int, not a string, and the
// `Append(value: string)` form takes a non-null literal.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           stringbuilder_capacity_and_append_accepted_s3_1) {
    const std::string src = program(
        "var sb := new StringBuilder(16); sb.Append(\"x\");");
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(src));
}

// §7.4 / stdlib: `Path.Combine` with two non-null string literals.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           path_combine_nonnull_accepted_s7_4) {
    const std::string src = program("var p := Path.Combine(\"a\", \"b\");");
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(src));
}

// §31.4: the `cause` of `(message: string, cause: Exception)` is non-nullable
// too. `new Exception("m", null)` compiled and the program died with SIGSEGV
// (exit 139) at the snapshot read.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           exception_cause_null_rejected_s31_4) {
    const std::string src = program("var e := new Exception(\"m\", null);");
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "null)",
        "the argument to `Exception` constructor is nullable but the target "
        "type is non-nullable (§7)"));
}

// --- Accept rows: §7.4-narrowed arguments at non-string overloads ---------
//
// The check reports nullness only. A nullable narrowed non-null by §7.4 is a
// non-null argument, and at a position whose overloads include a non-string
// type, overload resolution, not this check, decides the type. The first cut
// of #6782 ran a `string` assignability check here and falsely rejected each
// of these with a §5.1 error. Row names carry the review's probe ids.

// §3.1: `Append(value: Object)` with a narrowed `Foo?` (a19).
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           a19_append_narrowed_object_accepted_s7_4) {
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(
        "module M; class Foo end Foo; class C public static method T(c: bool): "
        "int var sb := new StringBuilder(); var o: Foo? := (if c then new "
        "Foo() else null); if o <> null then sb.Append(o); end; return 0; "
        "end T; end C;"));
}

// §3.1: `StringBuilder(capacity: int)` with a narrowed `int?` (a48).
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           a48_ctor_capacity_narrowed_int_accepted_s7_4) {
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(
        "module M; class C public static method T(ni: int?): int if ni <> null "
        "then var sb := new StringBuilder(ni); sb.Append(\"x\"); end; return 0; "
        "end T; end C;"));
}

// §9.1: the radix `ToString` with a narrowed `int?` (a07b).
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           a07b_int_tostring_radix_narrowed_accepted_s7_4) {
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(
        "module M; class C public static method T(ni: int?): int var n := 255; "
        "if ni <> null then var s := n.ToString(ni); end; return 0; end T; "
        "end C;"));
}

// §3.1: `Append(value: int)` with a narrowed `int?` (a08b).
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           a08b_append_narrowed_int_accepted_s7_4) {
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(
        "module M; class C public static method T(ni: int?): int var sb := new "
        "StringBuilder(); if ni <> null then sb.Append(ni); end; return 0; "
        "end T; end C;"));
}

// §3.1: `Append(value: double)` with a narrowed `double?` (a40).
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           a40_append_narrowed_double_accepted_s7_4) {
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(
        "module M; class C public static method T(c: bool): int var dn: double? "
        ":= (if c then 1.5 else null); var sb := new StringBuilder(); if dn <> "
        "null then sb.Append(dn); end; return 0; end T; end C;"));
}

// §3.1: `Append(value: char)` with a narrowed `char?` (a46).
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           a46_append_narrowed_char_accepted_s7_4) {
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(
        "module M; class C public static method T(c: bool): int var ch: char? "
        ":= (if c then 'A' else null); var sb := new StringBuilder(); if ch <> "
        "null then sb.Append(ch); end; return 0; end T; end C;"));
}

// §9.1: `Parse(s: ReadOnlyStringSpan)` with a narrowed span (a47).
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           a47_parse_narrowed_span_accepted_s7_4) {
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(
        "module M; class C public static method T(): int const src := \"42\"; "
        "var sp: ReadOnlyStringSpan? := (if src.Length > 0 then src.AsSpan(0) "
        "else null); if sp <> null then var n := int.Parse(sp); end; return 0; "
        "end T; end C;"));
}

// --- Accept rows: §7.4 "`readonly` fields narrow" -------------------------

// An instance readonly field read through a local (a20).
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           a20_readonly_field_parse_accepted_s7_4) {
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(
        "module M; class H public readonly F: string?; public constructor(f: "
        "string?) F := f; end constructor; end H; class C public static method "
        "T(s: string?): int var h := new H(s); if h.F <> null then var n := "
        "int.Parse(h.F); end; return 0; end T; end C;"));
}

// The same field read through `this` (a26).
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           a26_this_readonly_field_parse_accepted_s7_4) {
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(
        "module M; class H public readonly F: string?; public constructor(f: "
        "string?) F := f; end constructor; public method P(): int if this.F <> "
        "null then return int.Parse(this.F); end; return -1; end P; end H;"));
}

// A `static readonly` field (a42).
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           a42_static_readonly_field_parse_accepted_s7_4) {
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(
        "module M; class G public static readonly S: string? := \"12\"; end G; "
        "class C public static method T(): int if G.S <> null then var n := "
        "int.Parse(G.S); end; return 0; end T; end C;"));
}

// A readonly field at a standard exception's `message` (a29).
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           a29_readonly_field_exception_message_accepted_s7_4) {
    BEZEL_EXPECT_TRUE(acceptsWithoutErrors(
        "module M; class H public readonly F: string?; public constructor(f: "
        "string?) F := f; end constructor; end H; class C public static method "
        "T(s: string?): int var h := new H(s); if h.F <> null then using var ex "
        ":= new Exception(h.F); end; return 0; end T; end C;"));
}

// The control for the readonly rows: an UN-guarded readonly field read is
// still nullable, so it is still rejected. Readonly fields are honoured as
// §7.4 narrowing, not skipped.
BEZEL_TEST(sema_builtin_nullable_string_param_6782,
           readonly_field_unguarded_rejected_s7_4) {
    const std::string src =
        "module M; class H public readonly F: string?; public constructor(f: "
        "string?) F := f; end constructor; end H; class C public static method "
        "T(s: string?): int var h := new H(s); var n := int.Parse(h.F); "
        "return 0; end T; end C;";
    BEZEL_EXPECT_TRUE(rejectsAt(
        src, "h.F);",
        "the argument to `int.Parse` is nullable but the target type is "
        "non-nullable (§7)"));
}
