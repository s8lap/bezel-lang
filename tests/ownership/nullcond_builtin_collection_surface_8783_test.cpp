// fix #8783 (§7.5 / §26.5 rule 2, v0.359; stdlib §4.4-§4.11 / §4.14 /
// §4.17-§4.19): the `?.` spelling of a member OUTSIDE a builtin collection
// head's documented stdlib surface is REFUSED, exactly as the `.` spelling is.
//
// THE DEFECT (issue #8783, MEASURED on r30-c1 @ fb1de7ae6). The `.` spelling
// of a member a builtin collection head does not declare is refused with the
// standard library's own documented-surface diagnostic:
//
//   > no member `FirstNode` on `SortedSet` (stdlib §4.18) -- it is not part
//   > of this collection's documented surface (Add/Clear/Contains/Count/
//   > First/GetEnumerator/Last/Remove/TryAdd), a universal
//   > `ToString`/`Equals`/`GetHashCode`/`GetType` member (language spec
//   > §8.1), or a stdlib §5 LINQ extension operator
//
// while the `?.` spelling of the SAME name was ACCEPTED: the access was left
// UNTYPED, codegen lowered it to nothing behind the #3079 warning ("the
// argument of this `Console.WriteLine` failed to lower to a value, so the
// whole statement emits nothing -- not even the line terminator"), and the
// program exited 0 with EMPTY stdout. The `?.` spelling over a USER class was
// refused all along ("no member `Foo` on type `C` (§11.3, §12.1)"), so the
// gap was specific to the builtin collection heads.
//
// THE RULE. §7.5 (v0.359): "`x?.M` returns null if `x` is null; otherwise it
// performs the access. The result type is always nullable" -- the `?.`
// spelling PERFORMS the same access the `.` spelling does. §26.5 rule 2
// (v0.359): "Where neither rule 1 nor this search supplies the name, nothing
// supplies it: the access names no member and is a compile error at the name,
// the diagnostic naming the receiver's static type ... The outcome is the same
// for every member access on a value, not only a call -- a property or field
// read or write, and a method reference ... with a `?.` or postfix-`!`
// receiver read at its underlying non-nullable type (§7.5, §7.8)."
//
// ORACLE: `runAot` (the AOT pipeline). Every row asserts the SPECIFIC
// diagnostic TEXT (never the bare presence of an error, so a reject for an
// unrelated reason cannot pass), and the first row asserts that the `?.`
// spelling's diagnostic is the `.` spelling's diagnostic, VERBATIM -- the two
// spellings of one member may not disagree about it.
//
// Every row here is RED on the base (r30-c1 @ fb1de7ae6): on that tree the
// `?.` spelling compiled clean, the enclosing statement was dropped, and the
// program exited 0 with empty stdout. The documented `?.` members that must
// STAY admitted are corpus candidates (the harvest lane's material), not
// gtest rows -- a row that is green on the base pins nothing.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The stdlib's documented-surface diagnostic: "no member `M` on `Head`
// (stdlib §4.x) -- it is not part of this collection's documented surface
// (…), a universal `ToString`/… member (language spec §8.1), or a stdlib §5
// LINQ extension operator". Asserted by TEXT.
bool hasNoMemberDiagnostic(const JitResult& r, const char* member,
                           const char* head) {
    const std::string needle =
        std::string("no member `") + member + "` on `" + head + "`";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The stdlib SECTION the diagnostic cites ("stdlib §4.18").
bool citesStdlibSection(const JitResult& r, const char* section) {
    const std::string needle = std::string("stdlib \xC2\xA7") + section;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The stdlib's documented-surface diagnostic, WITHOUT the
// "<path>:<line>:<col>: " prefix the driver prints in front of it.
//
// The harness packs the driver's whole captured stderr into ONE diagnostic
// (a rejected program never runs, so there is no per-diagnostic location),
// and the two programs compared below are two DIFFERENT temporary files —
// their prefixes differ even though their messages do not. So the comparison
// is made on the message body: the text from "no member" to the end of that
// line.
std::string noMemberDiagnosticText(const JitResult& r, const char* member,
                                   const char* head) {
    const std::string needle =
        std::string("no member `") + member + "` on `" + head + "`";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        const std::size_t at = d.message.find(needle);
        if (at == std::string::npos) continue;
        std::size_t end = d.message.find('\n', at);
        if (end == std::string::npos) end = d.message.size();
        return d.message.substr(at, end - at);
    }
    return "";
}

// The program under test, parameterised by the head's CONSTRUCTOR, the
// receiver's declaration, the member name and the SPELLING (`.` or `?.`).
// `member` is a member name the head does NOT declare, so the program must
// be refused.
std::string undeclaredMemberProgram(const char* ctor, const char* decl,
                                    const char* member, const char* spelling) {
    return std::string(
               "module M; import System; import System.Collections.Generic;\n"
               "class C\n"
               "  public static method Main(): int\n"
               "    using var s := ") +
           ctor + ";\n" + "    var q: " + decl + " := s;\n" +
           "    Console.WriteLine(q" + spelling + member +
           ");\n" + "    return 0;\n" + "  end Main;\n" + "end C;\n";
}

}  // namespace

// ══════════════════════ #8783 — the `?.` spelling is refused ══════════════

// The issue's LITERAL repro, and the strongest form of the rule: the `?.`
// spelling's diagnostic must be the `.` spelling's diagnostic, VERBATIM.
//
// On the base this program COMPILED CLEAN (exit 0, empty stdout, only the
// #3079 internal warning) -- so the row is RED there.
BEZEL_TEST(nullcond_builtin_collection_surface_8783,
           sortedset_undeclared_member_refused_like_the_dot_spelling) {
    const JitResult dot = runAot(
        undeclaredMemberProgram("new SortedSet<int>()", "SortedSet<int>?",
                                "FirstNode", "."));
    const JitResult q = runAot(
        undeclaredMemberProgram("new SortedSet<int>()", "SortedSet<int>?",
                                "FirstNode", "?."));

    // The `.` spelling: the baseline the `?.` spelling must match. It is
    // refused on the base and on this arm -- asserted so a row cannot pass
    // by both spellings being refused for an unrelated reason.
    BEZEL_EXPECT_FALSE(dot.ran);
    BEZEL_EXPECT_TRUE(hasNoMemberDiagnostic(dot, "FirstNode", "SortedSet"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(dot, "4.18"));

    // The `?.` spelling: refused, naming the receiver's static type and the
    // stdlib section that declares the head's surface.
    BEZEL_EXPECT_FALSE(q.ran);
    BEZEL_EXPECT_TRUE(hasNoMemberDiagnostic(q, "FirstNode", "SortedSet"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(q, "4.18"));

    // ... and the two spellings' diagnostics are the SAME WORDS. §26.5
    // rule 2 makes the member lookup identical for the two spellings, so
    // the diagnostic must be too.
    BEZEL_EXPECT_TRUE(!noMemberDiagnosticText(dot, "FirstNode",
                                              "SortedSet").empty());
    BEZEL_EXPECT_TRUE(noMemberDiagnosticText(q, "FirstNode", "SortedSet") ==
                       noMemberDiagnosticText(dot, "FirstNode", "SortedSet"));
}

// The issue's own member name (`Foo`), over the issue's own head: a name NO
// section declares. RED on the base (compiled clean, statement dropped).
BEZEL_TEST(nullcond_builtin_collection_surface_8783,
           sortedset_undeclared_foo_member_refused) {
    const JitResult r = runAot(
        undeclaredMemberProgram("new SortedSet<int>()", "SortedSet<int>?",
                                "Foo", "?."));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasNoMemberDiagnostic(r, "Foo", "SortedSet"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.18"));
}

// A NEIGHBOURING head: the §4.8 dictionary. `Keys`/`Values`/`Count`/`Add`/
// `TryGetValue`/... are documented; `BogusMember` is not, and the `.`
// spelling refuses it (the #2536 catch-all's documented-surface diagnostic),
// so the `?.` spelling must refuse it with the same words.
//
// RED on the base: the `?.` spelling compiled clean and the statement was
// dropped.
BEZEL_TEST(nullcond_builtin_collection_surface_8783,
           dictionary_undeclared_member_refused) {
    const JitResult dot = runAot(
        undeclaredMemberProgram("new Dictionary<int, int>()",
                                "Dictionary<int, int>?", "BogusMember", "."));
    const JitResult q = runAot(
        undeclaredMemberProgram("new Dictionary<int, int>()",
                                "Dictionary<int, int>?", "BogusMember", "?."));
    BEZEL_EXPECT_FALSE(dot.ran);
    BEZEL_EXPECT_TRUE(hasNoMemberDiagnostic(dot, "BogusMember", "Dictionary"));
    BEZEL_EXPECT_FALSE(q.ran);
    BEZEL_EXPECT_TRUE(hasNoMemberDiagnostic(q, "BogusMember", "Dictionary"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(q, "4.8"));
    BEZEL_EXPECT_TRUE(
        noMemberDiagnosticText(q, "BogusMember", "Dictionary") ==
        noMemberDiagnosticText(dot, "BogusMember", "Dictionary"));
}

// The §4.6 linked list: `FirstNode`/`LastNode` ARE declared (stdlib §4.6),
// `BogusMember` is not. The `?.` spelling of the undeclared name must be
// refused, and the `?.` spelling of the DECLARED names must keep working
// (that half is a corpus candidate -- see the handback).
BEZEL_TEST(nullcond_builtin_collection_surface_8783,
           linkedlist_undeclared_member_refused) {
    const JitResult dot = runAot(
        undeclaredMemberProgram("new LinkedList<int>()", "LinkedList<int>?",
                                "BogusMember", "."));
    const JitResult q = runAot(
        undeclaredMemberProgram("new LinkedList<int>()", "LinkedList<int>?",
                                "BogusMember", "?."));
    BEZEL_EXPECT_FALSE(dot.ran);
    BEZEL_EXPECT_TRUE(hasNoMemberDiagnostic(dot, "BogusMember", "LinkedList"));
    BEZEL_EXPECT_FALSE(q.ran);
    BEZEL_EXPECT_TRUE(hasNoMemberDiagnostic(q, "BogusMember", "LinkedList"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(q, "4.6"));
    BEZEL_EXPECT_TRUE(
        noMemberDiagnosticText(q, "BogusMember", "LinkedList") ==
        noMemberDiagnosticText(dot, "BogusMember", "LinkedList"));
}

// The §4.4 array family: `Array<T>`'s `.` spelling is refused by its OWN
// per-family block (#2457 slice 1), whose wording differs from the
// per-head table's; the `?.` spelling is refused by the table's gate (the
// same gate the `?.` CALL form reads, fix #8348). The VERDICT must agree --
// the two spellings may not disagree about whether the name is a member.
//
// RED on the base: the `?.` spelling compiled clean and the statement was
// dropped.
BEZEL_TEST(nullcond_builtin_collection_surface_8783,
           array_undeclared_member_refused) {
    const std::string program =
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<int>();\n"
        "    var q: Array<int>? := a;\n"
        "    Console.WriteLine(q?.BogusMember);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    const JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasNoMemberDiagnostic(r, "BogusMember", "Array"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.4"));
}
