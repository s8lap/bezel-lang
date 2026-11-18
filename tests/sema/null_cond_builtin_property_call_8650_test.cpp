// Spec-derived regression test for issue #8650.
//
// THE SPEC.
//
// spec §26.5 rule 1 (v0.357): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` — of any arity, applicability, or
// accessibility — instance resolution is committed and extension blocks are
// **never consulted** for that name on that receiver. If the committed instance
// lookup then fails (no applicable overload, §22.5/§22.6, or the member is
// inaccessible), the call is a compile error citing the instance member(s); it
// does not fall back to an extension". The same section adds: "The outcome is
// the same for every member access on a value, not only a call — a property or
// field read or write, and a method reference, which resolves its member
// exactly as the call does (§27.6), with a `?.` or postfix-`!` receiver read
// at its underlying non-nullable type (§7.5, §7.8)."
//
// spec §7.5 (v0.357): "`x?.M` returns null if `x` is null; otherwise it
// performs the access." — the `?.` spelling performs the SAME access the `.`
// spelling does, so the two spellings of ONE member may not disagree.
//
// spec §27.1 / §27.7 (v0.357): only a function-typed property can be called.
//
// THE DEFECT (#8650).
//
// stdlib §4.4 declares `public pure readonly property Count: int;` on
// `Array<T>`, §4.8 the same on `Dictionary<K, V>`, §4.10 on `Set<T>`. So
// `a.Count()` names the PROPERTY and has no candidate — the house #7543
// refusal ("`Count` is a property of `Array`, not a method …").
//
// The `?.` spelling disagreed: `a?.Count()` on an `Array<int>?` was COMPILED
// and answered the element count, because the `?.` path resolved the LINQ
// extension `IEnumerable<T>.Count()` (stdlib §5) instead of committing to the
// instance member by name the way §26.5 rule 1 requires. MEASURED on the base
// tip: `a?.Count() ?? -1` prints 3 while `a.Count()` is refused.
//
// THE FIX.
//
// The `?.` arm of the built-in-property table now reads the SAME flag the `.`
// arm reads (`BuiltinPropertyDecl::refusesCall`), so the two spellings of ONE
// member can no longer disagree: `a?.Count()` is refused with the same house
// diagnostic as `a.Count()`.
//
// The rule-corpus rows that pinned the old behaviour (§7.5's `s7_5_0009`,
// `s7_5_0010`, `s7_5_0011`, `s7_5_0012`, `s7_5_0013` — `expect: run` /
// `expect: reject` over `a?.Count()` on an `Array<int>?`) change verdict with
// this fix; they are REPORTED, not re-banked (the RULE decides, the corpus
// vehicle rode on the old behaviour).

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
using bezel::test::runAot;

namespace {

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

// The same oracle WITHOUT the "it is the only error" clause, for the
// ARGUMENT position: the `?.` spelling's result type is nullable by §7.5, so
// the house ALSO reports "the argument to `Console.WriteLine` is nullable but
// the target type is non-nullable (§7)" at the same place. (The `.` spelling
// has no such follow-on — a refused `.` call has no type at all — so this
// cascade belongs to the `?.` spelling, not to the refusal.)
void expectRejectedWithText(const std::string& program, std::uint32_t line,
                            const std::string& expected) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(errorTextAtLine(r, line), expected);
}

// The EXACT diagnostic the fix introduces — the #7543 wording, with the
// stdlib's own declaration note and the remedy clause (the property READ is a
// modelled spelling on every head here, so the remedy is offered).
std::string propertyCallMessage(const std::string& member,
                                 const std::string& owner,
                                 const std::string& section,
                                 const std::string& type) {
    std::string msg = "`" + member + "` is a property of `" + owner +
                      "`, not a method (stdlib \xC2\xA7" + section + ": `" +
                      member + ": " + type + "`), so this call has no candidate: "
                      "the lookup commits to the instance member (\xC2\xA7""26.5 "
                      "rule 1), and only a function-typed property can be "
                      "called (\xC2\xA7""27.1, \xC2\xA7""27.7)";
    msg += " \xE2\x80\x94"" read it without parentheses, `." + member + "`";
    return msg;
}

// A program whose `Main` body is `prelude` followed by the call on the last
// line. The call's source line is returned with the program.
struct Prog {
    std::string source;
    std::uint32_t callLine;
};

// The receiver declarations + the call, on their own lines.
Prog prog(const std::string& recvDecl, const std::string& call) {
    std::string src = "module M; import System; import System.Linq;\n";  // 1
    src += "class C\n";                                                   // 2
    src += "  public static method Main(): int\n";                        // 3
    src += "    " + recvDecl + "\n";                                      // 4
    src += "    " + call + "\n";                                          // 5
    src += "    return 0;\n";                                             // 6
    src += "  end Main;\n";                                               // 7
    src += "end C;\n";                                                    // 8
    return Prog{src, 5};
}

}  // namespace

// ===========================================================================
// spec §7.5 — the `?.` spelling performs the SAME access the `.` spelling
// does, so the two spellings of ONE member may not disagree.
// ===========================================================================

// stdlib §4.4: `Array<T>` declares `pure readonly property Count: int`. The
// `?.` spelling names the same PROPERTY the `.` spelling names, so the call
// has no candidate (§26.5 rule 1, §27.1, §27.7).
BEZEL_TEST(null_cond_builtin_property_call_8650,
           array_null_cond_count_call_rejected_spec_7_5) {
    Prog p = prog("var a: Array<int>? := [1, 2, 3];", "var n := a?.Count();");
    expectRejectedWith(
        p.source, p.callLine,
        propertyCallMessage("Count", "Array", "4.4", "int"));
}

// stdlib §4.8: `Dictionary<K, V>` declares `pure readonly property Count: int`.
BEZEL_TEST(null_cond_builtin_property_call_8650,
           dictionary_null_cond_count_call_rejected_spec_7_5) {
    Prog p = prog("var d: Dictionary<string, int>? := new Dictionary<string, int>();",
                  "var n := d?.Count();");
    expectRejectedWith(
        p.source, p.callLine,
        propertyCallMessage("Count", "Dictionary", "4.8", "int"));
}

// stdlib §4.10: `Set<T>` declares `pure readonly property Count: int`.
BEZEL_TEST(null_cond_builtin_property_call_8650,
           set_null_cond_count_call_rejected_spec_7_5) {
    Prog p = prog("var s: Set<int>? := new Set<int>();", "var n := s?.Count();");
    expectRejectedWith(
        p.source, p.callLine,
        propertyCallMessage("Count", "Set", "4.10", "int"));
}

// stdlib §4.4: `Array<T>` declares `pure readonly property Capacity: int`.
BEZEL_TEST(null_cond_builtin_property_call_8650,
           array_null_cond_capacity_call_rejected_spec_7_5) {
    Prog p = prog("var a: Array<int>? := [1, 2, 3];",
                  "var n := a?.Capacity();");
    expectRejectedWith(
        p.source, p.callLine,
        propertyCallMessage("Capacity", "Array", "4.4", "int"));
}

// The #6529 repro A shape verbatim: the receiver is NOT nullable, and the `?.`
// spelling still names the `Array<T>` PROPERTY.
BEZEL_TEST(null_cond_builtin_property_call_8650,
           array_null_cond_count_call_non_nullable_receiver_rejected_spec_7_5) {
    Prog p = prog("var a: Array<int> := [1, 2, 3];", "var n := a?.Count();");
    expectRejectedWith(
        p.source, p.callLine,
        propertyCallMessage("Count", "Array", "4.4", "int"));
}

// The same call in EXPRESSION position. The `?.` spelling's result type is
// nullable (§7.5), so the house reports the nullability follow-on too — see
// `expectRejectedWithText`.
BEZEL_TEST(null_cond_builtin_property_call_8650,
           dictionary_null_cond_count_call_expression_position_rejected_spec_7_5) {
    Prog p = prog("var d: Dictionary<string, int>? := new Dictionary<string, int>();",
                  "Console.WriteLine(d?.Count());");
    expectRejectedWithText(
        p.source, p.callLine,
        propertyCallMessage("Count", "Dictionary", "4.8", "int"));
}

// ===========================================================================
// THE TWO BOUNDARY CONTROLS — base-green on BOTH arms (each names the wrong
// fix it catches).
// ===========================================================================

// BOUNDARY CONTROL 1. The property READ is a modelled spelling and stays
// admitted: `a?.Count` (no parentheses) reads the `Array<T>` property. Catches
// a wrong fix that gated the READ on the same flag (it would refuse a spelling
// the standard library documents).
BEZEL_TEST(null_cond_builtin_property_call_8650,
           array_null_cond_property_read_still_admitted_spec_7_5) {
    Prog p = prog("var a: Array<int>? := [1, 2, 3];",
                  "var n: int := a?.Count ?? -1; Console.WriteLine(n);");
    auto r = runAot(p.source);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// BOUNDARY CONTROL 2. A `?.` CALL of a METHOD-typed member is still legal:
// `e?.Count()` over an `IEnumerable<int>?` names the LINQ extension
// `IEnumerable<T>.Count()` (stdlib §5), because `IEnumerable<T>` declares no
// `Count` member of its own (§26.5 rule 1 commits to the instance member only
// where one exists). Catches a wrong fix that refused EVERY `?.` call on a
// built-in head.
BEZEL_TEST(null_cond_builtin_property_call_8650,
           enumerable_null_cond_count_call_still_admitted_spec_5) {
    std::string src = "module M; import System; import System.Linq;\n";  // 1
    src += "class C\n";                                                   // 2
    src += "  public static method Main(): int\n";                        // 3
    src += "    var a: Array<int> := [1, 2, 3];\n";                       // 4
    src += "    var e: IEnumerable<int>? := a;\n";                        // 5
    src += "    var n := e?.Count();\n";                                 // 6
    src += "    Console.WriteLine(n ?? -1);\n";                           // 7
    src += "    return 0;\n";                                             // 8
    src += "  end Main;\n";                                               // 9
    src += "end C;\n";                                                    // 10
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}
