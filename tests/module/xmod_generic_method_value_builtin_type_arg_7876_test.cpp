// Regression test for issue #7876 (§14.1 / §2.8.2 / §2.8.3 rule 4, v0.355).
//
// An importing module calls an imported generic method whose type argument is
// a VALUE BUILTIN (`DateTime`, `TimeSpan`, …). The defining module shipped
// only the i32-PLACEHOLDER template for that method, so the call runs that
// template with the value read as an `int`.
//
// The #6779 gate is the located reject for exactly that shape — it reports
// every non-`int` type argument the placeholder cannot carry. But it read a
// Named ref with no `declaration` as "an open type parameter" (the caller's
// own unbound parameter, which is not a concrete binding and is therefore not
// the gate's to report). A VALUE BUILTIN reaches TypeResolution with
// `declaration = null` ON PURPOSE (#1463 / §4.2 — `new T(...)` lowering keys
// off its absence), so the gate SKIPPED it.
//
// MEASURED on a30bb11d (base): `L.Uniq(xs)` over `Array<DateTime>` compiled
// and ran, printing 2 where the same-module twin printed 1 (the placeholder
// template's `Set<int>` read the DateTime's storage as an `int`, so equal
// ticks were not the same value); `L.Uniq<DateTime>(xs)` (the explicit
// spelling) printed 2 as well. `L.Idx` / `L.Same` (a bare-`T` formal) were
// already rejected, by the #1943 width gate.
//
// §14.1 (*Instantiation and its site*): "a call to a generic member at
// explicit or inferred type arguments" is an instantiation site. §2.8.2
// clause 2: "An external instantiation behaves identically to a same-module
// instantiation of the same generic at the same arguments", and §2.8.2's
// *No restrictions beyond visibility*: "An implementation unable to carry
// some body shape in its interface format must reject the export, or the
// import, with a diagnostic naming the shape; it must not alter the
// instantiation's semantics." §2.8.3 rule 4: `is` tests and patterns over
// the instantiation "match identically regardless of which artifact produced
// the value".
//
// The rows below:
//   * the three generics at `T = DateTime`, both spellings — REJECTED now;
//   * the same-module twin — still runs and still counts correctly (the
//     positive control: the shape is genuinely admitted in one module);
//   * the admitting twin `T = int` across the boundary — still green;
//   * the non-generic `Array<DateTime>` method in the same library — still
//     green (it is not a generic, so no placeholder is involved);
//   * `Idx` / `Same` at `T = DateTime` — still rejected (by the #1943 width
//     gate), i.e. this fix changes no existing verdict;
//   * a RECORD type argument — still rejected (this fix changes no existing
//     verdict).
//
// The oracle is `runAotModules` (the AOT path, fork-isolated
// `Driver::compileFile`); the in-process JIT harness is deprecated.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;

namespace {

const char* kLib7876 =
    "module Lib7876;\n"
    "public class L\n"
    "  public static method Uniq<T>(xs: Array<T>): int\n"
    "    var s := new Set<T>();\n"
    "    for var x in xs do _ := s.Add(x); end;\n"
    "    return s.Count;\n"
    "  end Uniq;\n"
    "  public static method UniqDt(xs: Array<DateTime>): int\n"
    "    var s := new Set<DateTime>();\n"
    "    for var x in xs do _ := s.Add(x); end;\n"
    "    return s.Count;\n"
    "  end UniqDt;\n"
    "  public static method Dist<T>(xs: Array<T>): int "
    "return xs.Distinct().Count(); end Dist;\n"
    "  public static method Idx<T>(xs: Array<T>, x: T): int "
    "return xs.IndexOf(x); end Idx;\n"
    "  public static method Same<T>(a: T, b: T): bool return a = b; end Same;\n"
    "  public static method DictCount<T>(xs: Array<T>): int\n"
    "    var d := new Dictionary<T, int>();\n"
    "    for var x in xs do d[x] := 1; end;\n"
    "    return d.Count;\n"
    "  end DictCount;\n"
    "end L;\n"
    "public record Pt(X: int, Y: int);\n";

// The importer. `decls` are the locals under test and `stmt` the statement;
// the DateTimes are IDENTICAL constructions, so the expected count is 1
// whatever the equality of two DateTimes is, and the program never depends
// on the host time zone.
std::string appSource(const std::string& decls, const std::string& stmt) {
    return "module App7876; import Lib7876;\n"                // 1
           "public class Program\n"                          // 2
           "  public static method Main(): int\n"            // 3
           "    return Run(); end Main;\n"                    // 4
           "  public static method Run(): int\n"             // 5
           "    " + decls + "\n"                             // 6
           "    " + stmt + "\n"                              // 7
           "    return 0;\n"
           "  end Run;\n"
           "end Program;\n";
}

// The three equal DateTimes the DateTime rows use.
const char* kDtDecls =
    "var d := new DateTime(2020,1,2,3,4,5); "
    "var xs: Array<DateTime> := [d, d, d];";

JitResult runApp(const std::string& app) {
    return bezel::test::runAotModules({{"Lib7876", kLib7876}, {"App7876", app}},
                                      "App7876");
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

// The imported generic must be REFUSED: the #6779 located reject, naming
// the type argument that cannot cross the boundary as the `int` placeholder.
void expectReject(const std::string& decls, const std::string& stmt,
                  const std::string& marker, const std::string& method,
                  const std::string& typeName) {
    const std::string app = appSource(decls, stmt);
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
std::string runOut(const std::string& decls, const std::string& stmt) {
    JitResult r = runApp(appSource(decls, stmt));
    if (!r.ran || r.exitCode != 0)
        return "<<fail ran=" + std::to_string(r.ran) +
               " rc=" + std::to_string(r.exitCode) + ">>";
    return r.stdoutText;
}

}  // namespace

// ── The issue's rows: the three generics at T=DateTime, INFERRED ──────────
// (§14.5 phase 1: the `Array<T>` formal binds T structurally to DateTime.)

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           uniq_datetime_inferred_rejected_at_argument_14_5) {
    expectReject(kDtDecls, "Console.WriteLine(L.Uniq(xs));", "xs));", "Uniq",
                 "System.DateTime");
}

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           dist_datetime_inferred_rejected_at_argument_14_5) {
    expectReject(kDtDecls, "Console.WriteLine(L.Dist(xs));", "xs));", "Dist",
                 "System.DateTime");
}

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           dictcount_datetime_inferred_rejected_at_argument_14_5) {
    expectReject(kDtDecls, "Console.WriteLine(L.DictCount(xs));", "xs));",
                 "DictCount", "System.DateTime");
}

// ── The same three, EXPLICIT type argument (§14.5: a full explicit list
// supplies the binding and no inference runs) ─────────────────────────────

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           uniq_datetime_explicit_rejected_at_argument_14_5) {
    expectReject(kDtDecls, "Console.WriteLine(L.Uniq<DateTime>(xs));", "xs));",
                 "Uniq", "System.DateTime");
}

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           dictcount_datetime_explicit_rejected_at_argument_14_5) {
    expectReject(kDtDecls, "Console.WriteLine(L.DictCount<DateTime>(xs));",
                 "xs));", "DictCount", "System.DateTime");
}

// ── The sibling value builtin: the same defect, the same fix ──────────────

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           uniq_timespan_inferred_rejected_at_argument_14_5) {
    expectReject(
        "var ts: Array<TimeSpan> := [new TimeSpan(1,2,3), "
        "new TimeSpan(1,2,3)];",
        "Console.WriteLine(L.Uniq(ts));", "ts));", "Uniq", "System.TimeSpan");
}

// ── The same-module twin: the shape is genuinely admitted in one module ───
// (the positive control — §2.8.2 clause 2's "behaves identically to a
// same-module instantiation" is what the cross-module rows must reach.)

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           uniq_datetime_same_module_control_14_1) {
    const std::string src =
        "module M7876; "
        "public class L "
        "  public static method Uniq<T>(xs: Array<T>): int "
        "    var s := new Set<T>(); "
        "    for var x in xs do _ := s.Add(x); end; "
        "    return s.Count; "
        "  end Uniq; "
        "end L; "
        "public class Program "
        "  public static method Main(): int "
        "    var d := new DateTime(2020,1,2,3,4,5); "
        "    var xs: Array<DateTime> := [d, d, d]; "
        "    Console.WriteLine(L.Uniq(xs)); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    JitResult r = bezel::test::runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// ── The admitting twin: `T = int` across the boundary stays green ─────────

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           uniq_int_cross_module_control_14_5) {
    BEZEL_EXPECT_EQ(runOut("var xs: Array<int> := [1, 1, 2];",
                            "Console.WriteLine(L.Uniq(xs));"),
                    std::string("2\n"));
}

// ── A NON-generic `Array<DateTime>` method in the same library stays green ─
// (no type parameter, so no placeholder is involved).

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           uniq_datetime_non_generic_control_14_1) {
    BEZEL_EXPECT_EQ(runOut(kDtDecls, "Console.WriteLine(L.UniqDt(xs));"),
                    std::string("1\n"));
}

// ── A caller's OWN type parameter named like a built-in is NOT a built-in ─
// (`class Box<Guid>` — §14.1; #8174 / #6530 record the resolver's answer per
// (spelling, scope), so the spelling `Guid` here names the PARAMETER.) This
// is the shape the fix's discriminator must not capture: at `Box<int>` the
// binding is `int` (admitted), and the template pass has no concrete binding
// at all — so the gate stands down for it in both.

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           caller_type_param_named_like_builtin_not_reported_14_1) {
    const std::string app =
        "module App7876; import Lib7876;\n"
        "public class Box<Guid>\n"
        "  public constructor() end constructor;\n"
        "  public method Go(xs: Array<Guid>): int => L.Uniq(xs);\n"
        "end Box;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var b := new Box<int>();\n"
        "    var xs: Array<int> := [1, 1, 2];\n"
        "    Console.WriteLine(b.Go(xs));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    JitResult r = runApp(app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// The same caller's type parameter, at a concrete binding the placeholder
// cannot carry: reported at the argument, naming the CONCRETE type.

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           caller_type_param_named_like_builtin_at_char_rejected_14_5) {
    const std::string app =
        "module App7876; import Lib7876;\n"
        "public class Box<Guid>\n"
        "  public constructor() end constructor;\n"
        "  public method Go(xs: Array<Guid>): int => L.Uniq(xs);\n"
        "end Box;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var b := new Box<char>();\n"
        "    var xs: Array<char> := ['a', 'a', 'b'];\n"
        "    Console.WriteLine(b.Go(xs));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    JitResult r = runApp(app);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "calling imported generic method `Uniq` with a type argument whose "
           "representation is not the compiled `int` (i32) placeholder"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "#6779"));
}

// ── No existing verdict changes: `Idx` / `Same` at T=DateTime stay rejected
// (the #1943 width gate), and a RECORD type argument stays rejected ────────

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           idx_datetime_still_rejected_14_1) {
    JitResult r = runApp(appSource(kDtDecls, "Console.WriteLine(L.Idx(xs, d));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "calling imported generic method `Idx` with a type argument whose "
           "representation is not the compiled `int` (i32) placeholder"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "#1943"));
}

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           same_datetime_still_rejected_14_1) {
    JitResult r = runApp(appSource(kDtDecls, "Console.WriteLine(L.Same(d, d));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "calling imported generic method `Same` with a type argument whose "
           "representation is not the compiled `int` (i32) placeholder"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "#1943"));
}

BEZEL_TEST(xmod_generic_method_value_builtin_type_arg_7876,
           record_type_argument_still_rejected_14_5) {
    const std::string app =
        "module App7876; import Lib7876;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var ps: Array<Pt> := [new Pt(1,2), new Pt(1,2)];\n"
        "    Console.WriteLine(L.Uniq(ps));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    JitResult r = runApp(app);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "calling imported generic method `Uniq` with a type argument whose "
           "representation is not the compiled `int` (i32) placeholder"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "#6779"));
}
