// Spec-derived regression test for issue #8633.
//
// THE SPEC.
//
// stdlib §1 "Block-by-name" (docs/Bezel_Standard_Library.md, v0.357): "An
// instance member blocks a same-named extension, document-wide: extension
// resolution commits to instance members **by name** — any arity,
// applicability, or accessibility, inherited ones included (spec §26.5 rule 1)
// — so wherever a type of this document declares or inherits an instance member
// named `Count`, `Contains`, `First`, `Last`, `Reverse`, or `ToArray`, a call
// on that receiver resolves to the member and the same-named §5 operator is
// unreachable there by name".
//
// stdlib §4.1 (v0.357) declares the member:
//   public interface IReadOnlyCollection<out T>: IEnumerable<T>
//     pure readonly property Count: int;
//   end IReadOnlyCollection;
//   public interface IReadOnlyList<out T>: IReadOnlyCollection<T> ...
//   public interface ICollection<T>: IReadOnlyCollection<T> ...
//   public interface IList<T>: IReadOnlyList<T>, ICollection<T> ...
// — so `IReadOnlyList<T>`, `ICollection<T>` and `IList<T>` INHERIT the
// `Count` PROPERTY, and stdlib §4.2's `IOwningCollection<T>` /
// `IOwningList<T>` inherit it the same way.
//
// spec §26.5 rule 1 (v0.357): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` — of any arity, applicability, or
// accessibility — instance resolution is committed and extension blocks are
// **never consulted** for that name on that receiver. If the committed instance
// lookup then fails ..., the call is a compile error citing the instance
// member(s); it does not fall back to an extension".
//
// spec §27.1 (v0.357) admits only a FUNCTION-typed value as a callee, and
// §27.7 (v0.357) is the compiler's "only function-typed values are callable"
// rule. The committed member is the arity-0 `Count` PROPERTY, so the CALL
// spelling `c.Count()` — a call through the property's value — has NO
// candidate.
//
// THE DEFECT — MEASURED on the base tip (r30-c1 b770c3650, spec v0.357):
//   var c: IList<int> := a;  c.Count();      -> COMPILED, exit 0, printed 3
//   var c: IReadOnlyList<int> := a; c.Count(); -> COMPILED, exit 0, printed 3
//   var c: IOwningList<Box> := a; c.Count();   -> COMPILED, exit 0, printed 0
//                                                (behind the internal #2434
//                                                zero-initializer warning)
//   var c: IList<int>? := a;  c?.Count();      -> COMPILED, exit 0, printed 3
//   var c: IOwningList<Box>? := a; c?.Count(); -> COMPILED, exit 0, printed -1
//                                                (the same internal #2434
//                                                zero-initializer warning)
// while the CONCRETE `Array<int>` head refuses the very same call:
//   var a: Array<int> := [1,2,3]; a.Count();   -> REFUSED (fix #7543):
//     "`Count` is a property of `Array`, not a method (stdlib §4.4: `Count:
//     int`), so this call has no candidate: ..."
//
// The two spellings of ONE member may not disagree (spec §7.5: the `?.`
// spelling performs the SAME access the `.` spelling does — the rule #8582
// and #8583 record), so the `?.` twin is refused with the SAME diagnostic.
//
// THE REMEDY. The property READ is the stdlib's own spelling: `c.Count` (not
// `c.Count()`). It is a modelled spelling on the three ARRAY-BACKED heads
// (`IList`, `IReadOnlyList`, `IOwningList` — MEASURED: `c.Count` prints 3
// over an `IList<int>`, `var n := c.Count;` prints 3, `c.Count` prints 2 over
// an `IOwningList<Box>`), so the refusal names it. On the three POLYMORPHIC
// heads (`ICollection`, `IReadOnlyCollection`, `IOwningCollection`) the READ
// is refused by codegen's #853 fence (the concrete backing is erased at the
// interface), so no remedy is named there.
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
// stdlib's own declaration note. `withRemedy` is false on the three heads
// whose property READ is refused by codegen's #853 fence.
std::string countPropertyCallMessage(const std::string& owner,
                                      const std::string& section,
                                      bool withRemedy) {
    std::string msg = "`Count` is a property of `" + owner +
                      "`, not a method (stdlib \xC2\xA7" + section +
                      ": `Count: int`), so this call has no candidate: the "
                      "lookup commits to the instance member (\xC2\xA7""26.5 "
                      "rule 1), and only a function-typed property can be "
                      "called (\xC2\xA7""27.1, \xC2\xA7""27.7)";
    if (withRemedy) {
        msg += " \xE2\x80\x94"" read it without parentheses, `.Count`";
    }
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

// The owning family (stdlib §4.2): the receiver is an `OwningArray<Box>`
// viewed through one of the owning collection interfaces. The element is a
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
// stdlib §4.1 — the BORROWING collection interfaces. The `Count` PROPERTY is
// inherited from `IReadOnlyCollection<out T>` (stdlib §4.1), so the CALL
// spelling has no candidate.
// ===========================================================================

// stdlib §4.1 / §1 "Block-by-name": `IList<T>` inherits `Count` from
// `IReadOnlyCollection<T>`, so `c.Count()` names the PROPERTY.
BEZEL_TEST(collection_interface_count_property_call_8633,
           ilist_count_call_rejected_spec_4_1) {
    Prog p = prog("var c: IList<int> := a;", "var n := c.Count();");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("IList", "4.1", true));
}

// stdlib §4.1: `IReadOnlyList<T>` inherits `Count` the same way.
BEZEL_TEST(collection_interface_count_property_call_8633,
           ireadonlylist_count_call_rejected_spec_4_1) {
    Prog p = prog("var c: IReadOnlyList<int> := a;", "var n := c.Count();");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("IReadOnlyList", "4.1", true));
}

// stdlib §4.1: `ICollection<T>` inherits `Count` from
// `IReadOnlyCollection<T>`. Its property READ is refused by codegen's #853
// fence (the concrete backing is erased at the interface), so the refusal
// names no `.Count` remedy.
BEZEL_TEST(collection_interface_count_property_call_8633,
           icollection_count_call_rejected_spec_4_1) {
    Prog p = prog("var c: ICollection<int> := a;", "var n := c.Count();");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("ICollection", "4.1", false));
}

// stdlib §4.1: the read-only polymorphic head is refused the same way.
BEZEL_TEST(collection_interface_count_property_call_8633,
           ireadonlycollection_count_call_rejected_spec_4_1) {
    Prog p = prog("var c: IReadOnlyCollection<int> := a;",
                  "var n := c.Count();");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("IReadOnlyCollection", "4.1",
                                                false));
}

// ===========================================================================
// stdlib §4.2 — the OWNING collection interfaces.
// ===========================================================================

// stdlib §4.2: `IOwningList<T>` inherits the `Count` PROPERTY from
// `IReadOnlyCollection<T>`. Before the fix the call was COMPILED and printed
// 0 — a silent wrong answer behind the internal #2434 zero-initializer.
BEZEL_TEST(collection_interface_count_property_call_8633,
           iowninglist_count_call_rejected_spec_4_2) {
    Prog p = owningProg("var c: IOwningList<Box> := oa;",
                        "var n := c.Count();");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("IOwningList", "4.2", true));
}

// stdlib §4.2: the owning polymorphic head is refused the same way.
BEZEL_TEST(collection_interface_count_property_call_8633,
           iowningcollection_count_call_rejected_spec_4_2) {
    Prog p = owningProg("var c: IOwningCollection<Box> := oa;",
                        "var n := c.Count();");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("IOwningCollection", "4.2",
                                                false));
}

// ===========================================================================
// The SAME MEMBER IN EXPRESSION POSITION — before the fix the whole
// `Console.WriteLine` call was answered with the element count.
// ===========================================================================

BEZEL_TEST(collection_interface_count_property_call_8633,
           ilist_count_call_expression_position_rejected_spec_4_1) {
    Prog p = prog("var c: IList<int> := a;",
                  "Console.WriteLine(c.Count());");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("IList", "4.1", true));
}

// ===========================================================================
// spec §7.5 — the `?.` spelling performs the SAME access the `.` spelling
// does, so the two spellings of ONE member may not disagree.
// ===========================================================================

// stdlib §4.1 / spec §7.5: `c?.Count()` on an `IList<int>?` names the same
// inherited `Count` PROPERTY. Before the fix it was COMPILED and printed 3.
BEZEL_TEST(collection_interface_count_property_call_8633,
           ilist_null_cond_count_call_rejected_spec_7_5) {
    Prog p = prog("var c: IList<int>? := a;", "var n := c?.Count();");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("IList", "4.1", true));
}

// stdlib §4.1 / spec §7.5: the read-only list interface's `?.` twin.
BEZEL_TEST(collection_interface_count_property_call_8633,
           ireadonlylist_null_cond_count_call_rejected_spec_7_5) {
    Prog p = prog("var c: IReadOnlyList<int>? := a;",
                  "var n := c?.Count();");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("IReadOnlyList", "4.1", true));
}

// stdlib §4.2 / spec §7.5: the owning list interface's `?.` twin. Before the
// fix the call degraded to a zero-initialised value behind the internal #2434
// warning and printed -1.
BEZEL_TEST(collection_interface_count_property_call_8633,
           iowninglist_null_cond_count_call_rejected_spec_7_5) {
    Prog p = owningProg("var c: IOwningList<Box>? := oa;",
                        "var n := c?.Count();");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("IOwningList", "4.2", true));
}

// stdlib §4.1 / spec §7.5: the polymorphic head's `?.` twin.
BEZEL_TEST(collection_interface_count_property_call_8633,
           icollection_null_cond_count_call_rejected_spec_7_5) {
    Prog p = prog("var c: ICollection<int>? := a;", "var n := c?.Count();");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("ICollection", "4.1", false));
}

// stdlib §4.1 / spec §7.5: the read-only polymorphic head's `?.` twin.
BEZEL_TEST(collection_interface_count_property_call_8633,
           ireadonlycollection_null_cond_count_call_rejected_spec_7_5) {
    Prog p = prog("var c: IReadOnlyCollection<int>? := a;",
                  "var n := c?.Count();");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("IReadOnlyCollection", "4.1",
                                                false));
}

// stdlib §4.2 / spec §7.5: the owning polymorphic head's `?.` twin.
BEZEL_TEST(collection_interface_count_property_call_8633,
           iowningcollection_null_cond_count_call_rejected_spec_7_5) {
    Prog p = owningProg("var c: IOwningCollection<Box>? := oa;",
                        "var n := c?.Count();");
    expectRejectedWith(p.source, p.callLine,
                       countPropertyCallMessage("IOwningCollection", "4.2",
                                                false));
}
