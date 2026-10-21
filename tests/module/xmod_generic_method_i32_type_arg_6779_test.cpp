// Regression test for issue #6779 (§14.1 / §2.8.2 / §2.8.3 rule 4, v0.340).
//
// An importing module calls an imported generic method by binding it to the
// defining module's template body, compiled against the `int` (i32)
// placeholder. The #1943 gate rejected only a type argument whose LLVM WIDTH
// differs, so `char`, an enum and `uint` (i32 themselves) and `short`/`byte`
// (widened into the i32 slot) ran the template as an `int`: MEASURED on
// 283db84e, `H.Show<char>('a')` printed 97, `H.Show(Color.Green)` printed 1,
// and `H.Eq<X>(v, o)` with an equal boxed payload printed False for X = char,
// Color, uint, short.
//
// §14.1: a call to a generic member "at explicit or inferred type arguments"
// is an instantiation site. §2.8.2 clause 2: "An external instantiation
// behaves identically to a same-module instantiation of the same generic at
// the same arguments", and "An implementation unable to carry some body shape
// in its interface format must reject the export, or the import, with a
// diagnostic naming the shape; it must not alter the instantiation's
// semantics." §2.8.3 rule 4: `is` tests and patterns over a value "match
// identically regardless of which artifact produced the value". The fix
// extends the #1943 located reject to these type arguments.
//
// The same-module rows are controls: a same-module call is monomorphized and
// must keep printing the right answer.
//
// Not covered here (filed): an imported INTERFACE's generic method through
// interface dispatch (#6816), §14.5 Agreement on the imported channel
// (#6817).
//
// #6818 rows (below): an explicit NULLABLE value type argument (`char?`,
// `int?`, `Color?`, `long?`), whose argument crosses as the bare payload and
// so passed both the #5057 carrier check and the i32 test, and any non-`int`
// type bound only through a constructed formal (`Array<long>`,
// `Array<string>`, ...), which no width gate sees.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;

namespace {

const char* kLib6779 =
    "module Lib6779;\n"
    "public enum Color Red, Green; end Color;\n"
    "public class H\n"
    "  public static method Id<T>(a: T): Object => a;\n"
    "  public static method Show<T>(a: T): string => $\"{a}\";\n"
    "  public static method Eq<T>(a: T, b: Object): bool => a = b;\n"
    "  public static method ShowA<T>(xs: Array<T>): string => $\"{xs[0]}\";\n"
    "  public static method Pair<T>(a: T, b: T): string => $\"{a}{b}\";\n"
    "  public static method Count<T>(a: T): int => 7;\n"
    "  public static method D<T>(a: T): string => $\"[{default(T)}]\";\n"
    "end H;\n"
    "public class V\n"
    "  public constructor() end constructor;\n"
    "  public virtual method VShow<T>(a: T): string => $\"{a}\";\n"
    "end V;\n";

// The importer. Line 4 is `Go`, line 6 the `var o` line, line 7 the call.
std::string appSource(const std::string& objInit, const std::string& stmt) {
    return "module App6779; import Lib6779;\n"                          // 1
           "public class Program\n"                                    // 2
           "  public static method Main(): int\n"                      // 3
           "    return 0; end Main;\n"                                 // 4 (pad)
           "  public static method Go<U>(x: U): string => H.Show<U>(x);\n"  // 5
           "  public static method Run(): int\n"                       // 6
           "    var o: Object := " + objInit + ";\n"                   // 7
           "    " + stmt + "\n"                                        // 8
           "    return 0;\n"
           "  end Run;\n"
           "end Program;\n";
}

JitResult runApp(const std::string& app) {
    return bezel::test::runAotModules({{"Lib6779", kLib6779}, {"App6779", app}},
                                      "App6779");
}

// "<line>:<col>: " of the first occurrence of `marker` in `src` (1-based).
std::string locOf(const std::string& src, const std::string& marker) {
    std::size_t at = src.find(marker);
    if (at == std::string::npos) return "<marker not found>";
    std::size_t line = 1, lineStart = 0;
    for (std::size_t i = 0; i < at; ++i)
        if (src[i] == '\n') { ++line; lineStart = i + 1; }
    return ":" + std::to_string(line) + ":" + std::to_string(at - lineStart + 1) +
           ": ";
}

// The rejection, its exact location and the type it names. `marker` is the
// source text the diagnostic must point at (its first occurrence).
void expectReject(const std::string& objInit, const std::string& stmt,
                  const std::string& marker, const std::string& method,
                  const std::string& typeName) {
    // Main is never the entry here; make Run the program's work via Main.
    std::string app = appSource(objInit, stmt);
    JitResult r = runApp(app);
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string needle =
        locOf(app, marker) + "calling imported generic method `" + method +
        "` with a type argument whose representation is not the compiled "
        "`int` (i32) placeholder \xE2\x80\x94 here `" + typeName +
        "`, which is not `int`";
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, needle.c_str()));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "#6779"));
}

// A program that must run, printing `expected`.
std::string runOut(const std::string& objInit, const std::string& stmt) {
    std::string app = appSource(objInit, stmt);
    // Route Main through Run so the statement executes.
    const std::string from = "    return 0; end Main;\n";
    app.replace(app.find(from), from.size(), "    return Run(); end Main;\n");
    JitResult r = runApp(app);
    if (!r.ran || r.exitCode != 0)
        return "<<fail ran=" + std::to_string(r.ran) +
               " rc=" + std::to_string(r.exitCode) + ">>";
    return r.stdoutText;
}

}  // namespace

// ── Eq<X>(v, o): the issue's `= Object` repro, explicit type argument ──────

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, eq_char_rejected_at_type_arg_14_1) {
    expectReject("'a'", "Console.WriteLine(H.Eq<char>('a', o));",
                 "char>('a', o)", "Eq", "char");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, eq_enum_rejected_at_type_arg_14_1) {
    expectReject("Color.Green", "Console.WriteLine(H.Eq<Color>(Color.Green, o));",
                 "Color>(Color.Green, o)", "Eq", "Lib6779.Color");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, eq_uint_rejected_at_type_arg_14_1) {
    expectReject("5u", "Console.WriteLine(H.Eq<uint>(5u, o));", "uint>(5u, o)",
                 "Eq", "uint");
}

// `short` lowers to i16 and is widened into the i32 slot, so the reject is at
// the argument whose type it reads.
BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, eq_short_rejected_at_argument_14_1) {
    expectReject("short(5)", "Console.WriteLine(H.Eq<short>(short(5), o));",
                 "short(5), o)", "Eq", "short");
}

// ── Show<X>(v): interpolation ──────────────────────────────────────────────

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, show_char_rejected_at_type_arg_14_1) {
    expectReject("1", "Console.WriteLine(H.Show<char>('a'));", "char>('a')",
                 "Show", "char");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, show_enum_rejected_at_type_arg_14_1) {
    expectReject("1", "Console.WriteLine(H.Show<Color>(Color.Green));",
                 "Color>(Color.Green)", "Show", "Lib6779.Color");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, show_uint_rejected_at_type_arg_14_1) {
    expectReject("1", "Console.WriteLine(H.Show<uint>(5u));", "uint>(5u)",
                 "Show", "uint");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, show_short_rejected_at_argument_14_1) {
    expectReject("1", "Console.WriteLine(H.Show<short>(short(5)));",
                 "short(5))", "Show", "short");
}

// ── Id<X>(v): boxing to Object ─────────────────────────────────────────────

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, id_char_rejected_at_type_arg_14_1) {
    expectReject("1", "Console.WriteLine(H.Id<char>('a'));", "char>('a')",
                 "Id", "char");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, id_enum_rejected_at_type_arg_14_1) {
    expectReject("1", "Console.WriteLine(H.Id<Color>(Color.Green));",
                 "Color>(Color.Green)", "Id", "Lib6779.Color");
}

// The literal `5` crosses as an `int`; the explicit `uint` is what is read.
BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, id_uint_rejected_at_type_arg_14_1) {
    expectReject("1", "Console.WriteLine(H.Id<uint>(5));", "uint>(5)", "Id",
                 "uint");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, id_short_rejected_at_argument_14_1) {
    expectReject("1", "Console.WriteLine(H.Id<short>(short(5)));", "short(5))",
                 "Id", "short");
}

// ── Inferred type arguments (§14.1 "explicit or inferred") ─────────────────

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, inferred_char_rejected_at_argument_14_1) {
    expectReject("1", "Console.WriteLine(H.Show('a'));", "'a'));", "Show",
                 "char");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, inferred_enum_rejected_at_argument_14_1) {
    expectReject("1", "Console.WriteLine(H.Show(Color.Green));",
                 "Color.Green));", "Show", "Lib6779.Color");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, inferred_byte_rejected_at_argument_14_1) {
    expectReject("1", "var b: byte := byte(3); Console.WriteLine(H.Show(b));",
                 "b));", "Show", "byte");
}

// ── Inferred through a constructed formal, and agreeing bare-`T` formals ──
// (fix-forward: §14.5 phase 1 unifies `Array<T>` against `Array<char>`
// structurally; MEASURED on 23efeb4a these printed 97 and 1.)

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, inferred_array_char_rejected_at_argument_14_5) {
    expectReject("1",
                 "var xs := new Array<char>(); xs.Add('a'); "
                 "Console.WriteLine(H.ShowA(xs));",
                 "xs));", "ShowA", "char");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, inferred_array_enum_rejected_at_argument_14_5) {
    expectReject("1",
                 "var xs := new Array<Color>(); xs.Add(Color.Green); "
                 "Console.WriteLine(H.ShowA(xs));",
                 "xs));", "ShowA", "Lib6779.Color");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, inferred_pair_char_rejected_at_argument_14_5) {
    expectReject("1", "Console.WriteLine(H.Pair('a', 'b'));", "'a', 'b'",
                 "Pair", "char");
}

// ── A caller's own type parameter, bound at `char` by its specialization ───

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, caller_type_param_at_char_rejected_14_1) {
    expectReject("1", "Console.WriteLine(Go<char>('z'));", "U>(x)", "Show",
                 "char");
}

// ── A virtual generic method (the slot-call arm) and the `?.` arm ──────────

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, virtual_char_rejected_at_type_arg_14_1) {
    expectReject("1", "var v := new V(); Console.WriteLine(v.VShow<char>('q'));",
                 "char>('q')", "VShow", "char");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, null_conditional_char_rejected_at_type_arg_14_1) {
    // #6778 (§7.5, §7.1, v0.340): a `?.` result is nullable, so it coalesces
    // at `WriteLine`'s non-nullable parameter; the type argument is the
    // rejection under test.
    expectReject("1",
                 "var v: V? := new V(); Console.WriteLine(v?.VShow<char>('q') ?? \"\");",
                 "char>('q')", "VShow", "char");
}

// ── Controls: `int` crosses and runs ───────────────────────────────────────
// (`H.Id<int>(5)` is not a control: it prints null, #6803.)

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, int_control_runs_14_1) {
    BEZEL_EXPECT_EQ(
        runOut("5",
               "Console.WriteLine($\"{H.Eq<int>(5, o)} {H.Show<int>(5)} "
               "{H.Show(6)} {Go<int>(7)} {new V().VShow<int>(4)}\");"),
        std::string("True 5 6 7 4\n"));
}

// fix-forward (review of 23efeb4a): with an explicit list the WRITTEN binding
// decides, never the argument's own type. §14.5: "When a full explicit list
// is supplied, no inference runs"; the `short`/`byte` argument converts to
// `int` at the argument site (§5.1) and the `int` placeholder body is right.
// §2.8.2: cross-module use is not restricted relative to same-module use.
// MEASURED: 283db84e printed "5", "5", "True"; 23efeb4a rejected all three.

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, explicit_int_with_narrower_argument_runs_14_5) {
    BEZEL_EXPECT_EQ(
        runOut("3",
               "var s: short := short(5); "
               "Console.WriteLine($\"{H.Show<int>(short(5))} {H.Show<int>(s)} "
               "{H.Eq<int>(byte(3), o)}\");"),
        std::string("5 5 True\n"));
}

// `int` through a constructed formal, inferred and explicit, and the #1943
// T5 control (`Count<long>(99)`: a `long` type argument whose body never
// reads `T` keeps compiling and printing 7; the `long` explicit list stands
// down from this gate, and the width gate passes the i32 literal, #6803).
BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, int_array_and_count_long_control_14_5) {
    BEZEL_EXPECT_EQ(
        runOut("1",
               "var xs := new Array<int>(); xs.Add(4); "
               "Console.WriteLine($\"{H.ShowA(xs)} {H.ShowA<int>(xs)} "
               "{H.Pair(1, 2)} {H.Count<long>(99)}\");"),
        std::string("4 4 12 7\n"));
}

// ── Controls: the same calls within one module are monomorphized ───────────

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, same_module_char_enum_uint_short_control_14_1) {
    const std::string src =
        "module M; "
        "public enum Color Red, Green; end Color; "
        "public class H "
        "  public static method Id<T>(a: T): Object => a; "
        "  public static method Show<T>(a: T): string => $\"{a}\"; "
        "  public static method Eq<T>(a: T, b: Object): bool => a = b; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var oc: Object := 'a'; var oe: Object := Color.Green; "
        "    var ou: Object := 5u; var os: Object := short(5); "
        "    Console.WriteLine($\"{H.Id<char>('a')} {H.Show<char>('a')} {H.Eq<char>('a', oc)} {H.Show('b')}\"); "
        "    Console.WriteLine($\"{H.Id<Color>(Color.Green)} {H.Show(Color.Green)} {H.Eq<Color>(Color.Green, oe)}\"); "
        "    Console.WriteLine($\"{H.Id<uint>(5u)} {H.Show<uint>(5u)} {H.Eq<uint>(5u, ou)}\"); "
        "    Console.WriteLine($\"{H.Id<short>(short(5))} {H.Show<short>(short(5))} {H.Eq<short>(short(5), os)}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    JitResult r = bezel::test::runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("a a True b\n"
                                "M.Color M.Color True\n"
                                "5 5 True\n"
                                "5 5 True\n"));
}

// ── #6818: an explicit nullable value type argument ───────────────────────
// `char?` lowers to the `{i8, i32}` carrier, so the i32 test missed it, but
// the argument crosses as the bare payload, so #5057's carrier check missed
// it too. MEASURED on 1dee80c2: `Show<char?>(char(97))` printed 97 and
// `D<char?>` / `D<int?>(5)` / `D<Color?>` printed [0]; the same-module
// program prints `a` and [null] (control below). §2.8.2 clause 2, v0.340.

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, show_nullable_char_rejected_at_type_arg_6818) {
    expectReject("1", "Console.WriteLine(H.Show<char?>(char(97)));",
                 "char?>(char(97))", "Show", "char?");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, default_nullable_char_rejected_at_type_arg_6818) {
    expectReject("1", "Console.WriteLine(H.D<char?>(char(97)));",
                 "char?>(char(97))", "D", "char?");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, default_nullable_int_rejected_at_type_arg_6818) {
    expectReject("1", "Console.WriteLine(H.D<int?>(5));", "int?>(5)", "D",
                 "int?");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, default_nullable_enum_rejected_at_type_arg_6818) {
    expectReject("1", "Console.WriteLine(H.D<Color?>(Color.Green));",
                 "Color?>(Color.Green)", "D", "Lib6779.Color?");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, default_nullable_long_rejected_at_type_arg_6818) {
    expectReject("1", "Console.WriteLine(H.D<long?>(5));", "long?>(5)", "D",
                 "long?");
}

// A nullable-typed ARGUMENT is still #5057's: `Show<int?>(n)` keeps its
// carrier diagnostic, at the argument.
BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, nullable_int_argument_still_5057_6818) {
    const std::string app =
        appSource("1", "var n: int? := 5; Console.WriteLine(H.Show<int?>(n));");
    JitResult r = runApp(app);
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string needle =
        locOf(app, "n));") + "calling imported generic method `Show` with a "
        "nullable-value-typed argument (`T?`) that cannot cross the module "
        "boundary as a bare `int` placeholder";
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, needle.c_str()));
}

// ── #6818: a non-`int` type bound through a constructed formal ─────────────
// `Array<T>` is a pointer at every `T`, so no width gate sees the element.
// MEASURED on 1dee80c2, `ShowA` printed 705032704 for {5000000000L},
// pointer bits for {"hi"}, 0 for {2.5}, 1 for {true}; explicit
// `ShowA<long>` / `ShowA<string>` the same. Same-module control below.

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, inferred_array_long_rejected_at_argument_6818) {
    expectReject("1",
                 "var xs := new Array<long>(); xs.Add(5000000000L); "
                 "Console.WriteLine(H.ShowA(xs));",
                 "xs));", "ShowA", "long");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, inferred_array_string_rejected_at_argument_6818) {
    expectReject("1",
                 "var xs := new Array<string>(); xs.Add(\"hi\"); "
                 "Console.WriteLine(H.ShowA(xs));",
                 "xs));", "ShowA", "string");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, inferred_array_double_rejected_at_argument_6818) {
    expectReject("1",
                 "var xs := new Array<double>(); xs.Add(2.5); "
                 "Console.WriteLine(H.ShowA(xs));",
                 "xs));", "ShowA", "double");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, inferred_array_bool_rejected_at_argument_6818) {
    expectReject("1",
                 "var xs := new Array<bool>(); xs.Add(true); "
                 "Console.WriteLine(H.ShowA(xs));",
                 "xs));", "ShowA", "bool");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, explicit_array_long_rejected_at_argument_6818) {
    expectReject("1",
                 "var xs := new Array<long>(); xs.Add(5000000000L); "
                 "Console.WriteLine(H.ShowA<long>(xs));",
                 "xs));", "ShowA", "long");
}

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, explicit_array_string_rejected_at_argument_6818) {
    expectReject("1",
                 "var xs := new Array<string>(); xs.Add(\"hi\"); "
                 "Console.WriteLine(H.ShowA<string>(xs));",
                 "xs));", "ShowA", "string");
}

// ── #6818 control: the same calls within one module ────────────────────────

BEZEL_TEST(xmod_generic_method_i32_type_arg_6779, same_module_nullable_and_array_control_6818) {
    const std::string src =
        "module M; "
        "public enum Color Red, Green; end Color; "
        "public class H "
        "  public static method Show<T>(a: T): string => $\"{a}\"; "
        "  public static method D<T>(a: T): string => $\"[{default(T)}]\"; "
        "  public static method ShowA<T>(xs: Array<T>): string => $\"{xs[0]}\"; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine($\"{H.Show<char?>(char(97))} {H.D<char?>(char(97))} {H.D<int?>(5)} {H.D<Color?>(Color.Green)} {H.D<long?>(5)}\"); "
        "    var xl := new Array<long>(); xl.Add(5000000000L); "
        "    var xs := new Array<string>(); xs.Add(\"hi\"); "
        "    var xd := new Array<double>(); xd.Add(2.5); "
        "    var xb := new Array<bool>(); xb.Add(true); "
        "    Console.WriteLine($\"{H.ShowA(xl)} {H.ShowA(xs)} {H.ShowA(xd)} {H.ShowA(xb)} {H.ShowA<long>(xl)} {H.ShowA<string>(xs)}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    JitResult r = bezel::test::runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("a [null] [null] [null] [null]\n"
                                "5000000000 hi 2.5 True 5000000000 hi\n"));
}
