// Spec-derived regression tests for issue #8133 — §22.15 *Pure members*,
// "The pure body" rule 6 and "Intrinsic purity (the stdlib contract)"
// (language spec v0.355; the standard-library marker source is
// `docs/Bezel_Standard_Library.md`, v0.355).
//
// THE DEFECT — two receiver SPELLINGS the standard-library arm of rule 6 did
// not reach, both in `src/sema/PureChecker.cpp`'s `PureBodyWalker`.
//
// 1. The `?.` SPELLING of a member access. A `?.` member access is its own
//    AST node (`ast::NullCondMemberExpr`), and the walker had no arm for it:
//    the `MemberAccessExpr` arm — which carries the standard-library check
//    fix #7453 landed — never saw it. So a standard-library member reached
//    through `?.` was admitted unconditionally, while the SAME member
//    spelled with a dot was rejected: `sb?.Append("x")` compiled in a `pure`
//    body and `sb.Append("x")` did not. The USER-member twin (`u?.Hit()`)
//    was already rejected, because the `CallExpr` arm resolves the callee
//    symbol and asks `callTargetIsPure` — but a STANDARD-LIBRARY member
//    resolves to NO symbol at all (a built-in carries no AST declaration),
//    so the declaration-based predicates had nothing to ask.
//
// 2. The KEYWORD-TYPE receiver. §3.4's keyword alias names a TYPE, not a
//    value, so `string` in `string.Join(",", xs)` is a type name: the
//    typecheck pass attaches no value type to it and no TypeSymbol either,
//    and the standard-library arm could not name the receiver at all. The
//    static member of a keyword-aliased type was therefore never read
//    against the library's `pure` markers.
//
// §22.15 rule 6, verbatim: "A call — method, operator, accessor read, or
// constructor in the admitted positions — is permitted iff the member
// declared on the receiver's static type (or the statically resolved member,
// for statics and extensions) is `pure`-declared or pure-classified (below)."
// The rule says nothing about the RECEIVER SPELLING — `sb?.Append("x")` is
// the same member of the same type as `sb.Append("x")` — and §22.15
// "Intrinsic purity (the stdlib contract)" makes the standard library's own
// marker the authority for a standard-library member.
//
// THE ORACLE. Two, deliberately.
//
//   * The SEMA oracle (`parseAndCheckPureGated`) is the phase under test: it
//     runs lex + parse + symbols + types + expressions + the §22.15
//     pure-body checker, gated exactly as the driver gates them. Each REJECT
//     row asserts the EXACT diagnostic text and the LINE the diagnostic
//     lands on, so a row cannot pass by observing the absence of something.
//   * The AOT oracle (`runAot`) is the strict whole-program oracle: the real
//     `Driver::compileFile`, forked, then clang-link and native exec. Each
//     shape has one AOT row proving the driver agrees — the REJECT rows that
//     the program is refused with the same diagnostic, the ADMIT rows that
//     the program COMPILES, LINKS and RUNS (exit code 0), so "it compiled" is
//     never the whole assertion.
//
// The sources are written with real newlines so each rejected row pins the
// diagnostic to its own line.

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>

namespace {

// The SEMA oracle — the pure-body checker under test.
bezel::test::SemaResult pureCheck(std::string_view src) {
    return bezel::test::parseAndCheckPureGated(src);
}

// The diagnostics whose message contains `needle`.
std::vector<const bezel::Diagnostic*> diagnosticsContaining(
    const bezel::test::SemaResult& r, const std::string& needle) {
    std::vector<const bezel::Diagnostic*> out;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) out.push_back(&d);
    return out;
}

// The ONE diagnostic whose message contains `needle` (asserts there is
// EXACTLY one, so a row cannot pass on a stray diagnostic that happens to
// carry the same words).
const bezel::Diagnostic* theDiagnosticContaining(
    const bezel::test::SemaResult& r, const std::string& needle) {
    auto hits = diagnosticsContaining(r, needle);
    if (hits.size() != 1) return nullptr;
    return hits.front();
}

// The AOT oracle — the real driver, forked. On a compile reject the driver's
// stderr lands in ONE Error diagnostic, so the needle is matched against that
// text.
bool aotRejectsWith(std::string_view src, const std::string& needle) {
    auto r = bezel::test::runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The AOT oracle's ADMIT side: the program must compile, LINK and RUN, and
// exit 0. `r.ran` false would mean the compile or the link failed, so the
// exit-code assertion alone is not enough.
bool aotRunsWithExitZero(std::string_view src) {
    auto r = bezel::test::runAot(src);
    return r.ran && r.exitCode == 0;
}

// §22.15 rule 6's diagnostic for a STANDARD-LIBRARY member the library does
// not mark `pure` (fix #7453's own text).
constexpr const char* kStdlibNotPure =
    "the standard library does not declare `StringBuilder.Append` pure";
// §22.15 rule 6's diagnostic for a USER member that is not `pure`.
constexpr const char* kUserNotPure =
    "the resolved target `P` is not pure-declared";

}  // namespace

// =========================================================================
// ITEM 1 — the `?.` spelling of a standard-library member.
// =========================================================================

// The issue's own repro: `Console.ReadLine()?.Length` inside a `pure` body.
// `Console.ReadLine` is an I/O entry point, and §22.15 rule 6 excludes "every
// entry point of the … I/O surfaces … with no per-API enumeration". The `?.`
// spelling did not change the member, so it did not change the verdict.
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           nullcond_stdlib_io_member_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class Program\n"
        "  public static pure method Run(): int\n"
        "    var n := Console.ReadLine()?.Length;\n"
        "    return n ?? 0;\n"
        "  end Run;\n"
        "end Program;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "the standard library does not declare `Console.ReadLine` pure");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 4u);
}

// The same member spelled with a dot was already rejected (fix #7453's
// arm); the `?.` spelling was not. This row pins the PAIR: the `?.` twin of
// an already-rejected shape.
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           nullcond_stdlib_method_call_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method M(sb: StringBuilder?): int\n"
        "    sb?.Append(\"x\");\n"
        "    return 0;\n"
        "  end M;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kStdlibNotPure);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        // The reported operation is the CALL (the `?.` is a spelling of the
        // member access, and the member is a method).
        BEZEL_EXPECT_TRUE(
            d->message.find("call to a standard-library member") !=
            std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find("§22.15 rule 6") !=
                           std::string::npos);
        BEZEL_EXPECT_TRUE(d->location.line == 4u);
    }
}

// The PROPERTY twin of the row above: a standard-library property read
// through `?.`. stdlib §3.1 declares `StringBuilder.Length` with no marker
// (its sibling `Capacity` carries one), so rule 6's "iff" reading rejects
// it — as the plain spelling `sb.Length` already did.
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           nullcond_stdlib_property_read_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method M(sb: StringBuilder?): int? => sb?.Length;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "the standard library does not declare `StringBuilder.Length` pure");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(
            d->message.find("accessor read of a standard-library member") !=
            std::string::npos);
        BEZEL_EXPECT_TRUE(d->location.line == 3u);
    }
}

// The USER-MEMBER twin of the `?.` property read: a property with an
// AUTHORED body is not `pure`-declared (§22.15 "Members classified by rule":
// "Authored bodies always require the explicit keyword"), and the plain
// spelling `u.P` was already rejected by #3630's accessor-read arm. The
// `?.` spelling was not, because the `NullCondMemberExpr` node carried no
// arm at all.
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           nullcond_user_property_read_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class U\n"
        "  public property P: int => 2;\n"
        "end U;\n"
        "class C\n"
        "  public static pure method M(u: U?): int? => u?.P;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kUserNotPure);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 6u);
}

// The AOT arm of the `?.` rows: the driver refuses the program.
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           nullcond_stdlib_member_rejected_by_driver_spec_22_15) {
    BEZEL_EXPECT_TRUE(aotRejectsWith(
        "module M;\n"
        "class Program\n"
        "  public static pure method Run(): int\n"
        "    var n := Console.ReadLine()?.Length;\n"
        "    return n ?? 0;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Run());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "the standard library does not declare `Console.ReadLine` pure"));
}

// The ADMIT controls — the members the standard library DOES mark `pure`
// stay admitted through the `?.` spelling, and so do the classified ones.
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           nullcond_stdlib_pure_members_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method M(s: string?, a: Array<int>?): int\n"
        "    var n := s?.Length ?? 0;\n"
        "    var c := a?.Count ?? 0;\n"
        "    var b := s?.Contains(\"a\") ?? false;\n"
        "    return n + c + (if b then 1 else 0);\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticsContaining(r, "not pure").empty());
}

// The USER-MEMBER control: a `pure`-declared method and an auto-property
// (whose getter is pure BY CLASSIFICATION — §22.15 "Members classified by
// rule": "an auto-property getter is pure (a backing-field read)") stay
// admitted through `?.`.
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           nullcond_user_pure_members_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class U\n"
        "  public pure method Hit(): int => 1;\n"
        "  public readonly property P: int := 1;\n"
        "end U;\n"
        "class C\n"
        "  public static pure method M(u: U?): int\n"
        "    var n := u?.Hit() ?? 0;\n"
        "    var p := u?.P ?? 0;\n"
        "    return n + p;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticsContaining(r, "not pure").empty());
}

// The AOT arm of the ADMIT controls: the programs compile, LINK and RUN.
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           nullcond_stdlib_pure_members_run_spec_22_15) {
    BEZEL_EXPECT_TRUE(aotRunsWithExitZero(
        "module M;\n"
        "class C\n"
        "  public static pure method M(s: string?): int\n"
        "    var n := s?.Length ?? 0;\n"
        "    return n;\n"
        "  end M;\n"
        "end C;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(C.M(\"ab\"));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n"));
}

// =========================================================================
// ITEM 2 — the KEYWORD-TYPE receiver.
// =========================================================================

// `string.Join(",", xs)` — stdlib §9.6 declares `public static method
// Join(separator: string, items: IEnumerable<string>): string;` with NO
// `pure` marker, so rule 6's "iff" reading rejects it in a pure body.
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           keyword_type_static_member_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method M(a: Array<string>): string => string.Join(\",\", a);\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "the standard library does not declare `string.Join` pure");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(
            d->message.find("call to a standard-library member") !=
            std::string::npos);
        BEZEL_EXPECT_TRUE(d->location.line == 3u);
    }
}

// §3.4's PascalCase alias names the SAME type ("the keyword `string` and the
// standard-library type name `String` name the same type — they are
// interchangeable in source"), so `String.Join` is the same member and the
// same verdict.
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           keyword_type_alias_static_member_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method M(a: Array<string>): string => String.Join(\",\", a);\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "the standard library does not declare `String.Join` pure");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 3u);
}

// The AOT arm of the keyword-type rows.
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           keyword_type_static_member_rejected_by_driver_spec_22_15) {
    BEZEL_EXPECT_TRUE(aotRejectsWith(
        "module M;\n"
        "class C\n"
        "  public static pure method M(a: Array<string>): string => string.Join(\",\", a);\n"
        "end C;\n",
        "the standard library does not declare `string.Join` pure"));
}

// The ADMIT controls — the keyword-type members the standard library DOES
// mark `pure` (or declares `const`) stay admitted: `string.From` /
// `string.Repeat` (stdlib §9.6, `pure`), `string.IsNullOrWhiteSpace`
// (`pure`), and the `static const` reads of the primitive records
// (`int.MaxValue`, `double.MaxValue`, `char.MaxValue`, `decimal.MaxValue` —
// §22.15 rule 1: "static field, `static readonly`, and `const` reads").
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           keyword_type_pure_and_const_members_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method M(s: string): string\n"
        "    var a := string.From('a');\n"
        "    var b := string.Repeat('a', 2);\n"
        "    var c := string.IsNullOrWhiteSpace(s);\n"
        "    var d := int.MaxValue;\n"
        "    var e := double.MaxValue;\n"
        "    var f := char.MaxValue;\n"
        "    var g := decimal.MaxValue;\n"
        "    return a + b + (if c then \"\" else \"\") + d.ToString() + e.ToString() + f.ToString() + g.ToString();\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticsContaining(r, "not pure").empty());
}

// The AOT arm of the keyword-type ADMIT control.
BEZEL_TEST(pure_body_nullcond_stdlib_8133,
           keyword_type_pure_members_run_spec_22_15) {
    BEZEL_EXPECT_TRUE(aotRunsWithExitZero(
        "module M;\n"
        "class C\n"
        "  public static pure method M(): string => string.From('a');\n"
        "end C;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(C.M());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n"));
}
