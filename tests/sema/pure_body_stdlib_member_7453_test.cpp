// Spec-derived regression tests for issue #7453 — §22.15 *Pure members*,
// "The pure body" rule 6 and its prohibited list (language spec v0.353,
// §22.15; the stdlib marker source is `docs/Bezel_Standard_Library.md`,
// v0.350/v0.353).
//
// THE DEFECT. §22.15 rule 6: "A call — method, operator, accessor read, or
// constructor in the admitted positions — is permitted iff the member
// declared on the receiver's static type (or the statically resolved member,
// for statics and extensions) is `pure`-declared or pure-classified (below)."
// The compiler knows a USER member's purity from its declaration (the `pure`
// modifier) and an IMPORTED member's from the artifact interface
// (`MetaMember::isPure`). A STANDARD-LIBRARY member has neither: the
// compiler-built-in classes carry no AST declaration and no artifact, so the
// callee resolves to NO symbol, and `PureBodyWalker::callTargetIsPure`'s
// final arm ("Intrinsic / builtin calls (no AST decl) are admitted") admitted
// EVERY standard-library call. The same gap admitted every standard-library
// CONSTRUCTION: the `NewExpr` arm rejected a class only when
// `rt->declaration->kind == Class`, and a built-in class has no declaration.
//
// §22.15's answer is the library's own marker: "Intrinsic purity (the stdlib
// contract). A standard-library member whose body is runtime-provided may
// carry `pure` … The marker is the library's *stated contract*, normative
// for the implementation … and trusted by the checker rather than verified
// from source." And the prohibited list rejects "class construction — `new
// C(…)`" outright (admitted only at the throw operand).
//
// THE ORACLE. Every row asserts the EXACT diagnostic text §22.15's rule
// produces (and, for the rejected rows, the line the diagnostic lands on),
// so a row cannot pass by observing the absence of something. The ACCEPT
// rows are the controls: they assert that the members the standard library
// DOES mark `pure` (stdlib §2.2 `Math.Abs`, §4.4 `Array<T>.Count`, §9.6
// `string.Length`) still compile, and that the §22.15 throw carve-out still
// compiles.
//
// The sources are written with real newlines so each rejected row pins the
// diagnostic to its own line.

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
        bezel::sema::checkPureBodies(*r.sourceFile, *r.ctx);
    }
    return r;
}

// The diagnostics whose message contains `needle`.
std::vector<const bezel::Diagnostic*> diagnosticsContaining(
    const bezel::test::SemaResult& r, const std::string& needle) {
    std::vector<const bezel::Diagnostic*> out;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) out.push_back(&d);
    return out;
}

// The one diagnostic whose message contains `needle` (asserts there is
// EXACTLY one, so a row cannot pass on a stray diagnostic that happens to
// carry the same words).
const bezel::Diagnostic* theDiagnosticContaining(
    const bezel::test::SemaResult& r, const std::string& needle) {
    auto hits = diagnosticsContaining(r, needle);
    if (hits.size() != 1) return nullptr;
    return hits.front();
}

}  // namespace

// =========================================================================
// The REJECT rows: §22.15 rule 6 applies to the standard library.
// =========================================================================

// The issue's own repro, line by line: `Console.WriteLine(s)` inside a
// `pure` method is a call to a standard-library member the standard
// library does not mark `pure` (stdlib §2.1 declares no marker on it).
//
// The diagnostic names the MEMBER (the rule's own subject) and lands on the
// call's line.
BEZEL_TEST(pure_body_stdlib_member_7453,
           console_write_line_in_pure_body_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Program\n"
        "  public static pure method Shout(s: string)\n"
        "    Console.WriteLine(s);\n"
        "  end Shout;\n"
        "end Program;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "call to a standard-library member is not admitted in a pure body");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->message.find("Console.WriteLine") !=
                           std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find(
                              "the standard library does not declare "
                              "`Console.WriteLine` pure") != std::string::npos);
        // §22.15 rule 6 is the rule the diagnostic cites.
        BEZEL_EXPECT_TRUE(d->message.find("§22.15 rule 6") !=
                           std::string::npos);
        // The call is on line 4 of the source.
        BEZEL_EXPECT_TRUE(d->location.line == 4u);
    }
}

// The issue's second repro: `new StringBuilder()` is CLASS CONSTRUCTION,
// which §22.15's prohibited list rejects in a pure body ("class
// construction — `new C(…)` … except the throw operand below").
BEZEL_TEST(pure_body_stdlib_member_7453,
           stdlib_class_construction_in_pure_body_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Program\n"
        "  public static pure method Build(s: string): string\n"
        "    var b := new StringBuilder();\n"
        "    b.Append(s);\n"
        "    return b.ToString();\n"
        "  end Build;\n"
        "end Program;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "standard-library class construction is not admitted in a pure "
           "body");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->message.find("`new StringBuilder(...)`") !=
                           std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find("§22.15") != std::string::npos);
        // The construction is on line 4 of the source.
        BEZEL_EXPECT_TRUE(d->location.line == 4u);
    }
}

// The issue's third repro: `b.Append(s)` — a call to a standard-library
// METHOD the standard library does not mark `pure` (stdlib §3.1 declares no
// marker on `StringBuilder.Append`).
BEZEL_TEST(pure_body_stdlib_member_7453,
           stdlib_method_call_in_pure_body_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Program\n"
        "  public static pure method Build(s: string): string\n"
        "    var b := new StringBuilder();\n"
        "    b.Append(s);\n"
        "    return b.ToString();\n"
        "  end Build;\n"
        "end Program;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "the standard library does not declare `StringBuilder.Append` pure");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->message.find(
                              "call to a standard-library member is not "
                              "admitted in a pure body") != std::string::npos);
        // The call is on line 5 of the source.
        BEZEL_EXPECT_TRUE(d->location.line == 5u);
    }
}

// The issue's other measured shapes: `Console.Write(s)`, `Console.ReadLine()`
// (stdlib §2.1 — the console I/O surface; §22.15 rule 6 excludes "every
// entry point of the … I/O surfaces") and `DateTime.Now.Ticks` (stdlib
// §2.5 — the clock read). Each is a standard-library member call the
// library does not mark `pure`.
BEZEL_TEST(pure_body_stdlib_member_7453,
           stdlib_io_and_clock_members_in_pure_body_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Program\n"
        "  public static pure method Write(s: string)\n"
        "    Console.Write(s);\n"
        "  end Write;\n"
        "  public static pure method Read(): string?\n"
        "    return Console.ReadLine();\n"
        "  end Read;\n"
        "  public static pure method Now(): long\n"
        "    return DateTime.Now.Ticks;\n"
        "  end Now;\n"
        "end Program;\n");
    const bezel::Diagnostic* w = theDiagnosticContaining(
        r, "the standard library does not declare `Console.Write` pure");
    BEZEL_EXPECT_TRUE(w != nullptr);
    if (w) BEZEL_EXPECT_TRUE(w->location.line == 4u);

    const bezel::Diagnostic* rd = theDiagnosticContaining(
        r, "the standard library does not declare `Console.ReadLine` pure");
    BEZEL_EXPECT_TRUE(rd != nullptr);
    if (rd) BEZEL_EXPECT_TRUE(rd->location.line == 7u);

    const bezel::Diagnostic* n = theDiagnosticContaining(
        r, "the standard library does not declare `DateTime.Now` pure");
    BEZEL_EXPECT_TRUE(n != nullptr);
    if (n) BEZEL_EXPECT_TRUE(n->location.line == 10u);
}

// `new Array<int>()` is the same class construction, on a COLLECTION type.
BEZEL_TEST(pure_body_stdlib_member_7453,
           stdlib_collection_construction_in_pure_body_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Program\n"
        "  public static pure method F(): int\n"
        "    var a := new Array<int>();\n"
        "    return a.Count;\n"
        "  end F;\n"
        "end Program;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "standard-library class construction is not admitted in a pure "
           "body");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->message.find("`new Array(...)`") !=
                           std::string::npos);
        BEZEL_EXPECT_TRUE(d->location.line == 4u);
    }
}

// =========================================================================
// The ACCEPT rows: the members the standard library DOES mark `pure`, and
// the §22.15 throw carve-out, keep compiling.
// =========================================================================

// stdlib §2.2 `Math.Abs`, §4.4 `Array<T>.Count` and §9.6 `string.Length`
// carry the `pure` marker, so §22.15 rule 6 admits them in a pure body.
BEZEL_TEST(pure_body_stdlib_member_7453,
           stdlib_pure_members_in_pure_body_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Program\n"
        "  public static pure method F(a: Array<int>, s: string): int\n"
        "    return Math.Abs(a.Count) + s.Length;\n"
        "  end F;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The §22.6 string-indexer form of the same: stdlib §9.6 declares the
// `string` indexer `pure`, so `s[0]` is admitted in a pure body.
BEZEL_TEST(pure_body_stdlib_member_7453,
           stdlib_pure_string_indexer_in_pure_body_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Program\n"
        "  public static pure method F(s: string): char\n"
        "    return s[0];\n"
        "  end F;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The §22.15 throw carve-out: `new E(...)` is admitted as the immediate
// operand of a `throw` when E's constructor is `pure` and E is
// trivial-dispose (stdlib §31.4 declares every built-in exception's
// constructors `pure`). The carve-out is untouched by this change.
BEZEL_TEST(pure_body_stdlib_member_7453,
           throw_carve_out_still_admitted_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Program\n"
        "  public static pure method Checked(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new ArgumentException(\"negative\", nameof(x));\n"
        "    end;\n"
        "    return x;\n"
        "  end Checked;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The issue's own control: a USER member that is not `pure`-declared is
// still rejected, and the diagnostic still names the resolved target. The
// standard-library arm does not change the user-declared arm.
BEZEL_TEST(pure_body_stdlib_member_7453,
           user_impure_member_call_still_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Program\n"
        "  public static method Log(s: string) => Console.WriteLine(s);\n"
        "  public static pure method Shout(s: string)\n"
        "    Log(s);\n"
        "  end Shout;\n"
        "end Program;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "the resolved target `Log` is not pure-declared");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->message.find("is not admitted in a pure body") !=
                           std::string::npos);
        // The call is on line 5 of the source.
        BEZEL_EXPECT_TRUE(d->location.line == 5u);
    }
}
