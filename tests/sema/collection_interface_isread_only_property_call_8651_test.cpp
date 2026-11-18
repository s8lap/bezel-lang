// Spec-derived regression test for issue #8651.
//
// THE SPEC.
//
// stdlib §4.1 (v0.357) declares `IsReadOnly` as a PROPERTY:
//
//   public interface ICollection<T>: IReadOnlyCollection<T>
//     readonly property IsReadOnly: bool;
//     ...
//   end ICollection;
//
// so `ICollection<T>` DECLARES it; `IList<T>` (stdlib §4.1) extends
// `ICollection<T>` and inherits it, and `IOwningCollection<T>` (stdlib §4.2)
// declares the same member, which `IOwningList<T>` extends. FOUR heads carry
// the member — `ICollection`, `IList`, `IOwningCollection`, `IOwningList`.
// (`IReadOnlyCollection<T>` / `IReadOnlyList<T>` declare no `IsReadOnly`;
// the six heads of #8633 are the `Count` heads.)
//
// spec §26.5 rule 1 (v0.357): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` — of any arity, applicability, or
// accessibility — instance resolution is committed and extension blocks are
// **never consulted** for that name on that receiver. If the committed instance
// lookup then fails (no applicable overload, §22.5/§22.6, or the member is
// inaccessible), the call is a compile error citing the instance member(s)".
//
// spec §27.1 (v0.357) admits only a FUNCTION-typed value as a callee, and
// §27.7 (v0.357) is the compiler's "only function-typed values are callable"
// rule. The committed member is the arity-0 `IsReadOnly` PROPERTY of type
// `bool`, so the CALL spelling `c.IsReadOnly()` has NO candidate — the same
// #7543 refusal `c.Count()` gets.
//
// spec §7.5 (v0.357): "`x?.M` returns null if `x` is null; otherwise it
// performs the access." The `?.` spelling performs the SAME access the `.`
// spelling does (and §26.5 resolves a `?.` receiver "at its underlying
// non-nullable type"), so `c?.IsReadOnly()` is refused with the SAME
// diagnostic.
//
// THE DEFECT — MEASURED on the base tip (r30-c1 e69d8b050, spec v0.357):
//   var c: IList<int> := a;  c.IsReadOnly();  -> REJECTED, but with the
//     #2457 backstop: "`IsReadOnly` on built-in type `IList` produced no
//     value, and no dispatch arm emitted any code for this call. `IList` has
//     no user declaration, so its member surface is exactly what the standard
//     library documents for it: either `IsReadOnly` is not on that surface (a
//     misspelling), or it is documented but not implemented by this compiler
//     (#2457)" — a diagnostic that sends the reader looking for a misspelling
//     in a member the stdlib DOES declare.
//   var c: IList<int>? := a; c?.IsReadOnly(); -> COMPILED, exit 0, printed 0
//     (the silent degradation behind the internal #2434 zero-initializer
//     warning — a SILENT wrong answer, the worst defect class here).
//
// The REMEDY the house diagnostic offers IS a working spelling: the property
// READ is modelled on all four heads (stdlib §4.1 fixes the value —
// "`IsReadOnly` is `false` for every standard collection implementing
// `ICollection<T>`" — and codegen's #7017 arm lowers it to that constant), so
// `.IsReadOnly` prints `False`.
//
// Every row below is RED on the base commit and GREEN with the fix: each
// rejection row asserts the EXACT diagnostic text at the call's own source
// line and that it is the ONLY error (no cascade).

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

// The EXACT diagnostic the fix introduces — the #7543 wording, with the
// stdlib's own declaration note. The remedy clause is present on all four
// heads: the property READ is a modelled spelling on each of them (stdlib
// §4.1 fixes the value — "`IsReadOnly` is `false` for every standard
// collection implementing `ICollection<T>`" — and codegen's #7017 arm lowers
// the read to that constant), so `.IsReadOnly` works there.
std::string isReadOnlyPropertyCallMessage(const std::string& owner,
                                          const std::string& section) {
    std::string msg = "`IsReadOnly` is a property of `" + owner +
                      "`, not a method (stdlib \xC2\xA7" + section +
                      ": `IsReadOnly: bool`), so this call has no candidate: "
                      "the lookup commits to the instance member (\xC2\xA7""26.5 "
                      "rule 1), and only a function-typed property can be "
                      "called (\xC2\xA7""27.1, \xC2\xA7""27.7)";
    msg += " \xE2\x80\x94"" read it without parentheses, `.IsReadOnly`";
    return msg;
}

// A program whose `Main` body is `prelude` followed by the call on the last
// line. The call's source line is returned with the program.
struct Prog {
    std::string source;
    std::uint32_t callLine;
};

// The borrowing family: the receiver is an `Array<int>` viewed through one of
// stdlib §4.1's collection interfaces.
Prog prog(const std::string& recvDecl, const std::string& call) {
    std::string src = "module M; import System; import System.Linq;\n";  // 1
    src += "class C\n";                                                   // 2
    src += "  public static method Main(): int\n";                        // 3
    src += "    var a: Array<int> := [1,2,3];\n";                         // 4
    src += "    " + recvDecl + "\n";                                      // 5
    src += "    " + call + "\n";                                          // 6
    src += "    return 0;\n";                                             // 7
    src += "  end Main;\n";                                               // 8
    src += "end C;\n";                                                    // 9
    return Prog{src, 6};
}

// The owning family: the receiver is an `OwningArray<Box>` viewed through
// one of the owning collection interfaces (`IOwningCollection<T>`, stdlib §4.2). The element is a
// class, so the owning collection disposes it at scope end.
Prog owningProg(const std::string& recvDecl, const std::string& call) {
    std::string src = "module M; import System; import System.Linq;\n";  // 1
    src += "class Box public v: int := 0; end Box;\n";                   // 2
    src += "class C\n";                                                   // 3
    src += "  public static method Main(): int\n";                        // 4
    src += "    using var oa := new OwningArray<Box>();\n";              // 5
    src += "    oa.Add(new Box()); oa.Add(new Box());\n";                 // 6
    src += "    " + recvDecl + "\n";                                      // 7
    src += "    " + call + "\n";                                          // 8
    src += "    return 0;\n";                                             // 9
    src += "  end Main;\n";                                               // 10
    src += "end C;\n";                                                    // 11
    return Prog{src, 8};
}

}  // namespace

// ===========================================================================
// stdlib §4.1 — the BORROWING collection interfaces. `IsReadOnly` is declared
// on `ICollection<T>` and inherited by `IList<T>`, so the CALL spelling has
// no candidate.
// ===========================================================================

// stdlib §4.1 / §26.5 rule 1 / §27.1, §27.7: `IList<T>` INHERITS
// `IsReadOnly` from `ICollection<T>`, so `c.IsReadOnly()` names the PROPERTY.
// This is the issue's own repro.
BEZEL_TEST(collection_interface_isread_only_property_call_8651,
           ilist_isreadonly_call_rejected_spec_4_1) {
    Prog p = prog("var c: IList<int> := a;", "var b: bool := c.IsReadOnly();");
    expectRejectedWith(p.source, p.callLine,
                       isReadOnlyPropertyCallMessage("IList", "4.1"));
}

// stdlib §4.1: `ICollection<T>` DECLARES the member.
BEZEL_TEST(collection_interface_isread_only_property_call_8651,
           icollection_isreadonly_call_rejected_spec_4_1) {
    Prog p = prog("var c: ICollection<int> := a;",
                  "var b: bool := c.IsReadOnly();");
    expectRejectedWith(p.source, p.callLine,
                       isReadOnlyPropertyCallMessage("ICollection", "4.1"));
}

// ===========================================================================
// stdlib §4.2 — the OWNING collection interfaces.
// ===========================================================================

// stdlib §4.2: `IOwningCollection<T>` DECLARES the member.
BEZEL_TEST(collection_interface_isread_only_property_call_8651,
           iowningcollection_isreadonly_call_rejected_spec_4_2) {
    Prog p = owningProg("var c: IOwningCollection<Box> := oa;",
                        "var b: bool := c.IsReadOnly();");
    expectRejectedWith(p.source, p.callLine,
                       isReadOnlyPropertyCallMessage("IOwningCollection", "4.2"));
}

// stdlib §4.2: `IOwningList<T>` inherits it from `IOwningCollection<T>`.
BEZEL_TEST(collection_interface_isread_only_property_call_8651,
           iowninglist_isreadonly_call_rejected_spec_4_2) {
    Prog p = owningProg("var c: IOwningList<Box> := oa;",
                        "var b: bool := c.IsReadOnly();");
    expectRejectedWith(p.source, p.callLine,
                       isReadOnlyPropertyCallMessage("IOwningList", "4.2"));
}

// ===========================================================================
// spec §7.5 — the `?.` spelling performs the SAME access the `.` spelling
// does, so the two spellings of ONE member may not disagree.
// ===========================================================================

// stdlib §4.1 / spec §7.5: `c?.IsReadOnly()` names the same inherited
// PROPERTY. Before the fix it was COMPILED and printed 0 — a silent
// zero-initialised answer behind the internal #2434 warning.
BEZEL_TEST(collection_interface_isread_only_property_call_8651,
           ilist_null_cond_isreadonly_call_rejected_spec_7_5) {
    Prog p = prog("var c: IList<int>? := a;", "var b := c?.IsReadOnly();");
    expectRejectedWith(p.source, p.callLine,
                       isReadOnlyPropertyCallMessage("IList", "4.1"));
}

// stdlib §4.1 / spec §7.5: the polymorphic head's `?.` twin.
BEZEL_TEST(collection_interface_isread_only_property_call_8651,
           icollection_null_cond_isreadonly_call_rejected_spec_7_5) {
    Prog p = prog("var c: ICollection<int>? := a;", "var b := c?.IsReadOnly();");
    expectRejectedWith(p.source, p.callLine,
                       isReadOnlyPropertyCallMessage("ICollection", "4.1"));
}

// stdlib §4.2 / spec §7.5: the owning pair's `?.` twin.
BEZEL_TEST(collection_interface_isread_only_property_call_8651,
           iowningcollection_null_cond_isreadonly_call_rejected_spec_7_5) {
    Prog p = owningProg("var c: IOwningCollection<Box>? := oa;",
                        "var b := c?.IsReadOnly();");
    expectRejectedWith(p.source, p.callLine,
                       isReadOnlyPropertyCallMessage("IOwningCollection", "4.2"));
}

// ===========================================================================
// THE SAME MEMBER IN EXPRESSION POSITION.
// ===========================================================================

BEZEL_TEST(collection_interface_isread_only_property_call_8651,
           ilist_isreadonly_call_expression_position_rejected_spec_4_1) {
    Prog p = prog("var c: IList<int> := a;",
                  "Console.WriteLine(c.IsReadOnly());");
    expectRejectedWith(p.source, p.callLine,
                       isReadOnlyPropertyCallMessage("IList", "4.1"));
}
