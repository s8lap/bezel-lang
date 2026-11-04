// Spec-derived regression test for issues #7543 and #7984 -- a PROPERTY
// called as a METHOD (`o.N()`, `sb.Length()`, `arr.Count()`,
// `Environment.NewLine(1)`) was ACCEPTED, and codegen then degraded the
// un-lowerable callee (the internal #2434 zero-initializer / the #3079
// "failed to lower" warning) behind exit 0.
//
// THE SPEC.
//
// spec §26.5 rule 1 (v0.350): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` -- of any arity, applicability, or
// accessibility -- instance resolution is committed and extension blocks are
// **never consulted** for that name on that receiver. If the committed
// instance lookup then fails (no applicable overload, §22.5/§22.6, or the
// member is inaccessible, §13.1), the call is a compile error citing the
// instance member(s); it does not fall back to an extension."
//
// spec §27.1 (v0.350) admits only a FUNCTION-typed value as a callee -- "a
// parameter, local, field, or property of function type" -- and §27.7
// (v0.350) is the compiler's "only function-typed values are callable" rule
// (the same rule the local / parameter / field callees are refused under).
//
// So a PROPERTY of a NON-function type is NOT callable: `x.P(...)` is a call
// through the property's value, and that value is an `int` / a `bool` / a
// `string`, not a function. The lookup committed to the property (§26.5 rule
// 1), so the call has NO candidate.
//
// THE DEFECT -- MEASURED on the base tip (r30-c1 e5ad8c2f, spec v0.355):
//
//   using var o := new C();  o.N();     -> COMPILED, exit 0, with
//                                            "codegen cannot dispatch member
//                                             'N' on receiver of type 'C'"
//                                            (#605) and the #3079 warning
//   sb.Length()                        -> COMPILED, exit 0, PRINTED 2 -- the
//                                            property's value; the call was
//                                            silently degraded to the read
//   arr.Count()                        -> COMPILED, exit 0, PRINTED the
//                                            property's value
//   b.Length()  (OwningBuffer<T>)      -> COMPILED, exit 0, printed 4
//   Environment.NewLine(1)              -> COMPILED, exit 0, with the #3079
//                                            "failed to lower" warning
//   DateTime.UtcNow()                   -> COMPILED, exit 0
//   o?.N()      (the `?.` twin)         -> COMPILED, exit 0
//
// WHAT STAYS LEGAL, and is pinned by the CONTROL rows below:
//
//   * a property OF FUNCTION TYPE called -- `o.F()` where `F` is a
//     lambda-typed property. §27.1 names "a property of function type" among
//     the callable values;
//   * a METHOD that shares its name with a property ELSEWHERE -- the
//     receiver's own declared surface decides (§26.5 rule 1);
//   * an EXTENSION method -- §26.5 rule 2, consulted only where the
//     receiver's declared type declares no member of that name;
//   * `Count()` where the stdlib declares a METHOD -- §5's
//     `IEnumerable<T>` operator, which no receiver head in the table declares
//     as a property;
//   * a delegate-typed FIELD -- §27.7's own carve-out, and the twin of the
//     property case;
//   * the bare property READS -- the spelling the stdlib declares.
//
// The rejection rows assert the EXACT diagnostic text, at the call's own
// source line, and assert that the program did not run and that the driver
// reported exactly ONE error (no cascade).

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// The body always sits on line 4.
std::string prog(const std::string& body) {
    return "module M; import System;\n"    // 1
           "class C\n"                       // 2
           "  public static method Main(): int\n"  // 3
           "    " + body + "\n"             // 4
           "    return 0;\n"                 // 5
           "  end Main;\n"                   // 6
           "end C;\n";                       // 7
}

// The same, over a USER class that declares its own members. The body sits on
// line 5.
std::string progWithUserClass(const std::string& decl, const std::string& body) {
    return "module M; import System;\n"    // 1
           "class Foo\n"                    // 2
           "  " + decl + "\n"               // 3
           "end Foo;\n"                     // 4
           "class C\n"                      // 5
           "  public static method Main(): int\n"  // 6
           "    " + body + "\n"             // 7
           "    return 0;\n"                 // 8
           "  end Main;\n"                   // 9
           "end C;\n";                       // 10
}

// The same, with a `D` class to own. The body sits on line 5.
std::string progWithOwned(const std::string& body) {
    return "module M; import System;\n"    // 1
           "class D public v: int := 1; end D;\n"  // 2
           "class C\n"                      // 3
           "  public static method Main(): int\n"  // 4
           "    " + body + "\n"             // 5
           "    return 0;\n"                 // 6
           "  end Main;\n"                   // 7
           "end C;\n";                       // 8
}

// One `<path>:<line>:<col>: <text>` line of the driver's captured stderr.
struct DiagLine {
    std::uint32_t line = 0;
    std::string text;
};

// The AOT oracle packs the driver's ENTIRE captured stderr into ONE
// `Diagnostic` (a genuine compile reject), whose message is the driver's
// `<path>:<line>:<col>: <text>` diagnostic lines, one per line. This splits
// that blob back into its lines.
std::vector<DiagLine> diagnosticLines(const JitResult& r) {
    std::vector<DiagLine> out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::istringstream in(d.message);
        std::string raw;
        while (std::getline(in, raw)) {
            const std::size_t c1 = raw.find(':');
            if (c1 == std::string::npos) continue;
            const std::size_t c2 = raw.find(':', c1 + 1);
            if (c2 == std::string::npos) continue;
            const std::size_t c3 = raw.find(':', c2 + 1);
            if (c3 == std::string::npos) continue;
            DiagLine dl;
            dl.line = static_cast<std::uint32_t>(std::strtoul(
                raw.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10));
            dl.text = raw.substr(c3 + 1);
            if (!dl.text.empty() && dl.text[0] == ' ') dl.text.erase(0, 1);
            out.push_back(dl);
        }
    }
    return out;
}

// A warning / note line carries its severity as a prefix; an error line does
// not.
bool isErrorLine(const DiagLine& dl) {
    return dl.text.rfind("warning: ", 0) != 0 && dl.text.rfind("note: ", 0) != 0;
}

// The EXACT text of the error diagnostic the driver reported at `line`.
std::string errorTextAtLine(const JitResult& r, std::uint32_t line) {
    for (const auto& dl : diagnosticLines(r))
        if (dl.line == line && isErrorLine(dl)) return dl.text;
    return std::string();
}

// How many ERROR diagnostics the driver reported in all.
std::size_t errorLineCount(const JitResult& r) {
    std::size_t n = 0;
    for (const auto& dl : diagnosticLines(r))
        if (isErrorLine(dl)) ++n;
    return n;
}

// The rejection oracle: the program did NOT run, the driver rejected it at a
// REAL source location (never a crash / signal / timeout), the diagnostic on
// `line` reads EXACTLY as expected, and it is the ONLY error (a fix that
// produced a cascade would fail here).
void expectRejectedWith(const std::string& program, std::uint32_t line,
                        const std::string& expected) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(errorTextAtLine(r, line), expected);
    BEZEL_EXPECT_EQ(errorLineCount(r), std::size_t{1});
}

// The EXACT diagnostic the fix introduces. One shape for a USER-declared
// property (spec §23 declares the property) and one for a BUILT-IN property
// (the stdlib section declares it).
std::string userPropertyCallMessage(const std::string& member,
                                    const std::string& owner) {
    return "`" + member + "` is a property of `" + owner +
           "`, not a method (\xC2\xA7""23), so this call has no candidate: the "
           "lookup commits to the instance member (\xC2\xA7""26.5 rule 1), "
           "and only a function-typed property can be called (\xC2\xA7""27.1, "
           "\xC2\xA7""27.7) \xE2\x80\x94"" read it without parentheses, `." +
           member + "`";
}

std::string builtinPropertyCallMessage(const std::string& member,
                                       const std::string& owner,
                                       const std::string& section,
                                       const std::string& type) {
    return "`" + member + "` is a property of `" + owner +
           "`, not a method (stdlib \xC2\xA7" + section + ": `" + member +
           ": " + type +
           "`), so this call has no candidate: the lookup commits to the "
           "instance member (\xC2\xA7""26.5 rule 1), and only a "
           "function-typed property can be called (\xC2\xA7""27.1, "
           "\xC2\xA7""27.7) \xE2\x80\x94"" read it without parentheses, `." +
           member + "`";
}

// A program that compiles and runs.
void expectRuns(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A program that compiles, runs, and prints exactly `out`.
void expectStdout(const std::string& program, const std::string& out) {
    auto r = runAot(program);
    if (!r.ran) {
        std::cout << "    #7543: program did not run\n";
        for (const auto& d : r.diagnostics.diagnostics())
            std::cout << "    " << d.message << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ===========================================================================
// #7543 -- a USER-declared property called as a method.
// ===========================================================================

// The issue's own repro: `o.N()` where `N` is an `int` property.
BEZEL_TEST(property_called_as_method_7543,
           user_property_call_is_rejected_spec_26_5_rule_1_spec_27_7) {
    expectRejectedWith(
        progWithUserClass("public property N: int := 3;",
                          "var o := new Foo(); var b := o.N();"),
        7, userPropertyCallMessage("N", "Foo"));
}

// An ARGUMENT does not turn the property into a method: the call still has no
// candidate.
BEZEL_TEST(property_called_as_method_7543,
           user_property_call_with_an_argument_is_rejected_spec_27_7) {
    expectRejectedWith(
        progWithUserClass("public property N: int := 3;",
                          "var o := new Foo(); var b := o.N(1);"),
        7, userPropertyCallMessage("N", "Foo"));
}

// The `?.` twin of the same call.
BEZEL_TEST(property_called_as_method_7543,
           null_conditional_user_property_call_is_rejected_spec_27_7) {
    expectRejectedWith(
        progWithUserClass("public property N: int := 3;",
                          "var o0 := new Foo(); var o: Foo? := o0; "
                          "var b := o?.N();"),
        7, userPropertyCallMessage("N", "Foo"));
}

// An INHERITED property commits the lookup just the same (§26.5 rule 1: "If
// `x`'s declared type (or any base type) *declares* any instance member named
// `Foo` ...").
BEZEL_TEST(property_called_as_method_7543,
           inherited_user_property_call_is_rejected_spec_26_5_rule_1) {
    expectRejectedWith(
        "module M; import System;\n"      // 1
        "class Base public property N: int := 3; end Base;\n"  // 2
        "class Derived : Base end Derived;\n"  // 3
        "class C\n"                        // 4
        "  public static method Main(): int\n"  // 5
        "    var o := new Derived(); var b := o.N();\n"  // 6
        "    return 0;\n"                   // 7
        "  end Main;\n"                     // 8
        "end C;\n",                         // 9
        6, userPropertyCallMessage("N", "Base"));
}

// A `bool` property is the same shape as an `int` one.
BEZEL_TEST(property_called_as_method_7543,
           user_bool_property_call_is_rejected_spec_27_7) {
    expectRejectedWith(
        progWithUserClass("public property Flag: bool := true;",
                          "var o := new Foo(); var b := o.Flag();"),
        7, userPropertyCallMessage("Flag", "Foo"));
}

// ===========================================================================
// #7543 -- the BUILT-IN instance properties the issue lists.
// ===========================================================================

// `sb.Length()` -- stdlib §3.1 declares `public readonly property Length: int;`.
BEZEL_TEST(property_called_as_method_7543,
           stringbuilder_length_call_is_rejected_stdlib_3_1) {
    expectRejectedWith(
        "module M; import System; import System.Text;\n"  // 1
        "class C\n"                          // 2
        "  public static method Main(): int\n"  // 3
        "    var sb := new StringBuilder(); var n := sb.Length();\n"  // 4
        "    return 0;\n"                     // 5
        "  end Main;\n"                       // 6
        "end C;\n",                           // 7
        4, builtinPropertyCallMessage("Length", "StringBuilder", "3.1", "int"));
}

// `arr.Count()` -- stdlib §4.4 declares `public pure readonly property Count:
// int;`. The stdlib's own "Block-by-name" row (stdlib §1) says the same-named
// §5 operator is unreachable on an `Array<T>` receiver for exactly this
// reason, and names the composed spelling `coll.Where(pred).Count()` as the
// way to reach the operator pipeline.
BEZEL_TEST(property_called_as_method_7543,
           array_count_call_is_rejected_stdlib_4_4) {
    expectRejectedWith(
        prog("var a := new Array<int>(); var n := a.Count();"), 4,
        builtinPropertyCallMessage("Count", "Array", "4.4", "int"));
}

// `b.Length()` -- stdlib §4.14 declares `public pure readonly property Length:
// int;` on `OwningBuffer<T>`.
BEZEL_TEST(property_called_as_method_7543,
           owning_buffer_length_call_is_rejected_stdlib_4_14) {
    expectRejectedWith(
        prog("using var b := new OwningBuffer<int>(4); var n := b.Length();"),
        4, builtinPropertyCallMessage("Length", "OwningBuffer", "4.14", "int"));
}

// `r.Start()` -- stdlib §9.8 declares `public readonly property Start: T?;` on
// `Range<T>`. The property READ is modelled (MEASURED: `r.Start` prints 1 on a
// `1..5` range), and the CALL spelling of it degraded to nothing (#605 / #3079)
// before this fix.
BEZEL_TEST(property_called_as_method_7543,
           range_start_call_is_rejected_stdlib_9_8) {
    expectRejectedWith(
        prog("var r := 1..5; var n := r.Start();"), 4,
        builtinPropertyCallMessage("Start", "Range", "9.8", "T?"));
}

// ===========================================================================
// CONTROLS: what must still compile.
// ===========================================================================

// A property OF FUNCTION TYPE is callable -- §27.1 names "a property of
// function type" among the callable values.
BEZEL_TEST(property_called_as_method_7543,
           function_typed_property_call_still_compiles_spec_27_1) {
    expectStdout(
        progWithUserClass(
            "public property F: (int) -> int := lambda(x: int) => x + 1;",
            "var o := new Foo(); Console.WriteLine(o.F(4));"),
        "5\n");
}

// The `?.` twin of the same: a function-typed property is callable there too.
// (The printed value is NOT the subject of this row: the null-conditional call
// through a function-typed PROPERTY does not lower in codegen — the #605
// "codegen cannot dispatch member" warning and the #2434 degradation — which
// is a pre-existing codegen gap on the base tip, not this fix's subject. The
// row pins that the call is ADMITTED.)
BEZEL_TEST(property_called_as_method_7543,
           null_conditional_function_typed_property_call_still_compiles) {
    expectRuns(
        progWithUserClass(
            "public property F: (int) -> int := lambda(x: int) => x + 1;",
            "var o0 := new Foo(); var o: Foo? := o0; "
            "var v: int? := o?.F(4); Console.WriteLine((v ?? -1).ToString());"));
}

// A METHOD that shares its name with a property ELSEWHERE is still callable:
// the receiver's own declared surface decides (§26.5 rule 1).
BEZEL_TEST(property_called_as_method_7543,
           a_method_sharing_a_property_name_elsewhere_is_still_callable) {
    expectStdout(
        progWithUserClass("public method Count(): int return 42; end Count;",
                          "var o := new Foo(); "
                          "Console.WriteLine(o.Count().ToString());"),
        "42\n");
}

// A USER class that declares the name as a PROPERTY is still readable -- the
// bare read is the spelling §23 declares.
BEZEL_TEST(property_called_as_method_7543,
           user_property_read_still_compiles_spec_23) {
    expectStdout(
        progWithUserClass("public property N: int := 3;",
                          "var o := new Foo(); "
                          "Console.WriteLine(o.N.ToString());"),
        "3\n");
}

// A delegate-typed FIELD is callable -- §27.7's own carve-out, and the twin
// of the property case (the §27.7 field rejection #3153 / #1138 already
// exists).
BEZEL_TEST(property_called_as_method_7543,
           delegate_typed_field_call_still_compiles_spec_27_7) {
    expectStdout(
        progWithUserClass(
            "public f: (int) -> int := lambda(x: int) => x + 1;",
            "var o := new Foo(); Console.WriteLine(o.f(4).ToString());"),
        "5\n");
}

// `Count()` where the stdlib declares a METHOD is still callable: §5's
// `IEnumerable<T>` operator, and the receiver's declared surface declares no
// `Count` of its own (§26.5 rule 2, the extension blocks are consulted).
BEZEL_TEST(property_called_as_method_7543,
           linq_count_operator_still_compiles_stdlib_5) {
    expectStdout(
        prog("var a: Array<int> := [1,2,3]; var e: IEnumerable<int> := a; "
             "var n := e.Count(); Console.WriteLine(n.ToString());"),
        "3\n");
}

// The composed spelling the stdlib's own "Block-by-name" row names --
// `coll.Where(pred).Count()` -- reaches the operator pipeline: the receiver
// is an `IEnumerable<T>`, which declares no `Count`.
BEZEL_TEST(property_called_as_method_7543,
           composed_where_count_spelling_still_compiles_stdlib_5) {
    expectStdout(
        prog("var a: Array<int> := [1,2,3]; "
             "var n := a.Where(lambda(x: int) => x > 1).Count(); "
             "Console.WriteLine(n.ToString());"),
        "2\n");
}

// The BARE reads of the built-in properties still read the property's value.
BEZEL_TEST(property_called_as_method_7543,
           builtin_property_reads_still_compile) {
    expectStdout(
        "module M; import System; import System.Text;\n"  // 1
        "class C\n"                          // 2
        "  public static method Main(): int\n"  // 3
        "    var sb := new StringBuilder(); sb.Append(\"ab\");\n"  // 4
        "    Console.WriteLine(sb.Length.ToString());\n"  // 5
        "    var a := new Array<int>(); a.Add(7);\n"  // 6
        "    Console.WriteLine(a.Count.ToString());\n"  // 7
        "    return 0;\n"                     // 8
        "  end Main;\n"                       // 9
        "end C;\n",                           // 10
        "2\n1\n");
}

// An EXTENSION method is still reachable where the receiver's declared type
// declares no member of that name (§26.5 rule 2).
BEZEL_TEST(property_called_as_method_7543,
           an_extension_method_is_still_callable_spec_26_5_rule_2) {
    expectStdout(
        "module M; import System;\n"          // 1
        "extension Array<int> public method Twice(): int => 2; end extension;\n"  // 2
        "class C\n"                            // 3
        "  public static method Main(): int\n"  // 4
        "    var a := new Array<int>();\n"      // 5
        "    Console.WriteLine(a.Twice().ToString());\n"  // 6
        "    return 0;\n"                       // 7
        "  end Main;\n"                         // 8
        "end C;\n",                             // 9
        "2\n");
}

// A same-named extension does NOT rescue a call to a property the receiver's
// declared type declares (§26.5 rule 1: the extension blocks are never
// consulted for that name on that receiver).
BEZEL_TEST(property_called_as_method_7543,
           an_extension_does_not_rescue_a_builtin_property_call_spec_26_5) {
    expectRejectedWith(
        "module M; import System;\n"          // 1
        "extension Array<int> public method Count(): int => 99; end extension;\n"  // 2
        "class C\n"                            // 3
        "  public static method Main(): int\n"  // 4
        "    var a := new Array<int>(); var n := a.Count();\n"  // 5
        "    return 0;\n"                       // 6
        "  end Main;\n"                         // 7
        "end C;\n",                             // 8
        5, builtinPropertyCallMessage("Count", "Array", "4.4", "int"));
}

// A USER class that reuses a built-in's name is not the built-in: its own
// declared surface answers, so a METHOD of that name is still callable.
BEZEL_TEST(property_called_as_method_7543,
           a_user_class_named_like_a_builtin_keeps_its_own_method) {
    expectStdout(
        progWithUserClass("public method Count(): int return 42; end Count;",
                          "var o := new Foo(); "
                          "Console.WriteLine(o.Count().ToString());"),
        "42\n");
}

// A USER class that reuses a built-in's name and declares the PROPERTY of
// that name is refused -- the property is the user's own, and §27.1 / §27.7
// do not care which type declared it.
BEZEL_TEST(property_called_as_method_7543,
           a_user_class_property_is_refused_whatever_its_name) {
    expectRejectedWith(
        progWithUserClass("public property Count: int := 42;",
                          "var o := new Foo(); var n := o.Count();"),
        7, userPropertyCallMessage("Count", "Foo"));
}

// ===========================================================================
// The `?.` twin of a BUILT-IN property call is NOT refused -- and this is a
// PIN, not an omission. The conformance corpus, the spec's own executable
// rules, requires the `?.` CALL spelling of a built-in property to RUN:
// §7.5's rule cases `s7_5_0009`, `s7_5_0010`, `s7_5_0012` and `s7_5_0013`
// are `expect: run` cases over `a?.Count()` on an `Array<int>?` -- "With a
// non-null receiver the access is the call `Count()`, which yields the element
// count, 2" -- and #6529 (CLOSED) is the issue that made the spelling work.
// The `?.` form is a DIFFERENT AST node with its own resolver, so the `.`
// form's refusal does not reach it.
//
// A USER-declared property's `?.` twin IS refused -- see
// `null_conditional_user_property_call_is_rejected_spec_27_7` above.
BEZEL_TEST(property_called_as_method_7543,
           null_conditional_builtin_property_call_stays_admitted_conformance_7_5) {
    expectStdout(
        "module M; import System;\n"          // 1
        "class C\n"                            // 2
        "  public static method Main(): int\n"  // 3
        "    var a: Array<int>? := [4, 2];\n"  // 4
        "    Console.WriteLine((a?.Count() ?? -1).ToString());\n"  // 5
        "    return 0;\n"                       // 6
        "  end Main;\n"                         // 7
        "end C;\n",                             // 8
        "2\n");
}

// ===========================================================================
// The BOUNDARY of the rule -- the two surfaces it does NOT reach.
// ===========================================================================

// The eight collection INTERFACE heads of stdlib §4.1 / §4.2 are OUT of the
// refusal. #7446's own ruling is that on an `IList<T>` receiver the built-in
// answers the zero-arg property call -- `IList<T>` INHERITS `Count` from
// `IReadOnlyCollection<T>`, so the user's extension is unreachable and the
// built-in's count is the answer -- and the property READ on a
// declaration-less interface head is not modelled in the
// local-initialiser position (MEASURED: `var n := c.Count;` on an
// `IList<int>` compiles with the internal #3079 "failed to lower" warning and
// prints NOTHING -- #7090's family). Refusing the call there would take away
// the only working spelling for the count on an interface-typed receiver, so
// the zero-arg call stays admitted. The arity>0 call (`c.Count(pred)`) is
// already refused, by #7597's own arm.
BEZEL_TEST(property_called_as_method_7543,
           interface_head_zero_arg_property_call_stays_admitted_7446) {
    expectStdout(
        prog("var a: Array<int> := [1,2,3]; var c: IList<int> := a; "
             "var n := c.Count(); Console.WriteLine(n.ToString());"),
        "3\n");
}

// `StringBuilder.Capacity` (stdlib §3.1) is a DECLARED-BUT-UNIMPLEMENTED
// member: its READ is already refused ("not yet implemented by this
// compiler", #7661's open question), so the call spelling is not this fix's
// to take away -- the existing refusal stands.
BEZEL_TEST(property_called_as_method_7543,
           stringbuilder_capacity_call_keeps_the_unimplemented_read_refusal) {
    auto r = runAot(
        "module M; import System; import System.Text;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var sb := new StringBuilder();\n"
        "    var n := sb.Capacity();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not yet implemented by this compiler"));
}
