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
// trivial-dispose (language spec §31.4 declares every built-in exception's
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

// =========================================================================
// fix-forward (#7453 review): the ADMIT rows the first fix over-rejected.
//
// The first fix keyed the standard library's purity by a hand-written
// (spelled head, member) table and rejected every member access that was not
// a row. §22.15 admits a built-in member access for THREE reasons, not one,
// and the stdlib declares the primitive types ONCE per family; each row
// below is a program the spec admits, the reviewer's probe a02/a06/a07/a15/
// a20/a22/a29/a30 (spec / stdlib v0.355). Each asserts that the program
// compiles with NO "standard-library member" diagnostic at all.
// =========================================================================

namespace {

// No diagnostic of the §22.15 standard-library arm, and no error at all.
void expectStdlibArmAdmits(const bezel::test::SemaResult& r) {
    BEZEL_EXPECT_TRUE(diagnosticsContaining(r, "standard-library member")
                          .empty());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

}  // namespace

// a02 — §22.15 rule 1: "static field, `static readonly`, and `const` reads
// … Reads are not effects." stdlib §2.2 declares `public static const Pi:
// double` and `public static const Tau: double`.
BEZEL_TEST(pure_body_stdlib_member_7453,
           math_const_reads_in_pure_body_accepted_spec_22_15_rule_1) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(): double => Math.Pi + Math.Tau;\n"
        "end C;\n");
    expectStdlibArmAdmits(r);
}

// a06 — stdlib §9.1: "`SByte`, `Byte`, `Int16`, `UInt16`, `Int32`, `UInt32`,
// `Int64`, `UInt64`. The example below shows `Int32`; the others have
// analogous shapes", and `Int32` declares `public pure method ToString():
// string;` and `public static pure method Parse(s: string): int;`.
BEZEL_TEST(pure_body_stdlib_member_7453,
           small_integer_to_string_in_pure_body_accepted_spec_22_15_stdlib_9_1) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(b: byte, u: uint, sh: short, ul: ulong, "
        "s: string): string\n"
        "    var p := uint.Parse(s);\n"
        "    return b.ToString() + u.ToString() + sh.ToString() + "
        "ul.ToString();\n"
        "  end F;\n"
        "end C;\n");
    expectStdlibArmAdmits(r);
}

// a30 — the same family: `CompareTo` (`public pure method CompareTo(other:
// int): int;` on `Int32`, "analogous" on `Byte`).
BEZEL_TEST(pure_body_stdlib_member_7453,
           small_integer_compare_to_in_pure_body_accepted_spec_22_15_stdlib_9_1) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(s: string, b: byte): int => "
        "int(byte.Parse(s)) + b.CompareTo(b);\n"
        "end C;\n");
    expectStdlibArmAdmits(r);
}

// a07 — stdlib §9.2 `Float`: `public pure method ToString(): string;`
// ("`Float` has the analogous shape").
BEZEL_TEST(pure_body_stdlib_member_7453,
           float_members_in_pure_body_accepted_spec_22_15_stdlib_9_2) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(f: float, s: string): string\n"
        "    var n := float.IsNaN(f);\n"
        "    var g := float.Parse(s);\n"
        "    return f.ToString();\n"
        "  end F;\n"
        "end C;\n");
    expectStdlibArmAdmits(r);
}

// a15 — §22.15 *Members classified by rule*: a record's synthesized
// `Equals` is pure "iff every field's equality dispatch target is pure —
// primitives, …"; stdlib §9.1: "`Equals(other: int): bool` — auto-generated
// from `IEquatable<int>` per language spec §9.2 … The same convention
// applies to every primitive declaration in §9.1–§9.5."
BEZEL_TEST(pure_body_stdlib_member_7453,
           primitive_equals_in_pure_body_accepted_spec_22_15_classified) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(x: int, y: int, d: double): bool => "
        "x.Equals(y) or d.Equals(1.0);\n"
        "end C;\n");
    expectStdlibArmAdmits(r);
}

// a20 — spec §3.4: the keyword ALIASES the type, so `x: Int32` names the
// same stdlib §9.1 record as `x: int`, whose `ToString` is `pure`.
BEZEL_TEST(pure_body_stdlib_member_7453,
           int32_spelling_to_string_in_pure_body_accepted_spec_3_4_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(x: Int32): string => x.ToString();\n"
        "end C;\n");
    expectStdlibArmAdmits(r);
}

// a22 / a29 — stdlib §4.14 `public record Span<T>(Buffer: OwningBuffer<T>,
// Start: int, Length: int)` and the same for `ReadOnlySpan<T>`: `Start` and
// `Length` are positional record components, whose getters §22.15
// classifies ("an auto-property getter is pure (a backing-field read)").
BEZEL_TEST(pure_body_stdlib_member_7453,
           span_component_reads_in_pure_body_accepted_spec_22_15_classified) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(sp: Span<int>): int => sp.Length + "
        "sp.Start;\n"
        "  public static pure method G(sp: ReadOnlySpan<int>): int => "
        "sp.Length + sp.Start;\n"
        "end C;\n");
    expectStdlibArmAdmits(r);
}

// The reviewer's r18: a USER enum's member access is not a standard-library
// member, so the standard-library arm must not report it (the first fix
// added a second, mislabelled "the standard library does not declare
// `Color.…`" error after the lookup failure).
BEZEL_TEST(pure_body_stdlib_member_7453,
           user_enum_member_not_reported_as_stdlib_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "enum Color\n"
        "  Red, Green;\n"
        "end Color;\n"
        "class C\n"
        "  public static pure method F(s: string): Color? => "
        "Color.FromNameOrNull(s);\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(diagnosticsContaining(r, "standard-library member")
                          .empty());
}

// =========================================================================
// The REJECT rows the review measured, pinned: each member carries no
// `pure` marker in the standard library, so §22.15 rule 6 rejects it.
// =========================================================================

// stdlib §2.2: `Min`, `Max`, `Clamp`, `Sign` are declared without `pure`
// ("Not `pure`: INumber<T>'s operators are not pure-declared").
BEZEL_TEST(pure_body_stdlib_member_7453,
           math_min_max_clamp_sign_in_pure_body_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(a: int, b: int): int\n"
        "    return Math.Min(a, b) + Math.Max(a, b) + Math.Clamp(a, 0, b) + "
        "Math.Sign(a);\n"
        "  end F;\n"
        "end C;\n");
    for (const char* m : {"Math.Min", "Math.Max", "Math.Clamp", "Math.Sign"}) {
        const bezel::Diagnostic* d = theDiagnosticContaining(
            r, std::string("the standard library does not declare `") + m +
                   "` pure");
        BEZEL_EXPECT_TRUE(d != nullptr);
        if (d) BEZEL_EXPECT_TRUE(d->location.line == 4u);
    }
}

// stdlib §2.4 `public static readonly property ProcessorCount: int;` (an
// environment intercept point, no marker) and stdlib §4.8 `ContainsKey` (no
// marker).
BEZEL_TEST(pure_body_stdlib_member_7453,
           environment_and_dictionary_members_in_pure_body_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(): int => Environment.ProcessorCount;\n"
        "  public static pure method G(d: Dictionary<string, int>): bool => "
        "d.ContainsKey(\"x\");\n"
        "end C;\n");
    const bezel::Diagnostic* e = theDiagnosticContaining(
        r, "the standard library does not declare "
           "`Environment.ProcessorCount` pure");
    BEZEL_EXPECT_TRUE(e != nullptr);
    if (e) BEZEL_EXPECT_TRUE(e->location.line == 3u);
    const bezel::Diagnostic* k = theDiagnosticContaining(
        r, "the standard library does not declare `Dictionary.ContainsKey` "
           "pure");
    BEZEL_EXPECT_TRUE(k != nullptr);
    if (k) BEZEL_EXPECT_TRUE(k->location.line == 4u);
}

// stdlib §8.1 `public static method StartNew(): owned Stopwatch;` — a clock
// read (an intercept point) with no marker.
BEZEL_TEST(pure_body_stdlib_member_7453,
           stopwatch_start_new_in_pure_body_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "import System.Diagnostics;\n"
        "class C\n"
        "  public static pure method F(): int\n"
        "    var sw := Stopwatch.StartNew();\n"
        "    return 0;\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "the standard library does not declare `Stopwatch.StartNew` pure");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 5u);
}

// =========================================================================
// Fix-forward #7453 (third attempt): the throw EXPRESSION carve-out, and the
// positional components of the built-in positional records.
// =========================================================================

// §22.15 *The throw carve-out*: "Inside a pure body, `new E(args)` is
// admitted only as the immediate operand of a `throw` statement or `throw`
// expression, and only when (a) the invoked constructor of `E` is
// `pure`-declared … and (b) `E` is a trivial-dispose class", and "Every
// built-in exception type's `()` and `(message)`-form constructors … are
// `pure`". The expression spelling (review probe n33): the `ThrowExpr` arm
// walked its operand as an ordinary expression, so the `NewExpr` arm's
// standard-library class-construction rejection fired on the carve-out's
// own validation idiom.
BEZEL_TEST(pure_body_stdlib_member_7453,
           throw_expression_carve_out_admits_stdlib_exception_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(s: string?): string => s ?? throw new "
        "ArgumentException(\"null s\");\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(
        diagnosticsContaining(r, "standard-library class construction")
            .empty());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The same carve-out in a block body (review probe n36): the throw
// expression is the right operand of `??` in a local's initializer.
BEZEL_TEST(pure_body_stdlib_member_7453,
           throw_expression_carve_out_in_block_body_admitted_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(s: string?): string\n"
        "    var t := s ?? throw new InvalidOperationException(\"missing\");\n"
        "    return t;\n"
        "  end F;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(
        diagnosticsContaining(r, "standard-library class construction")
            .empty());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The carve-out reaches a USER exception class in the expression spelling
// too (review probe n35, rejected on every earlier arm): `MyError` derives
// from `Exception` (trivial-dispose, condition (b)) and declares a `pure`
// constructor (condition (a)), so `s ?? throw new MyError(…)` is admitted
// exactly as the statement form `throw new MyError(…);` already was.
BEZEL_TEST(pure_body_stdlib_member_7453,
           throw_expression_carve_out_admits_user_pure_ctor_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "public class MyError: Exception\n"
        "  public pure constructor(message: string)\n"
        "    base(message);\n"
        "  end constructor;\n"
        "end MyError;\n"
        "class C\n"
        "  public static pure method F(s: string?): string => s ?? throw new "
        "MyError(\"null s\");\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(
        diagnosticsContaining(r, "class construction").empty());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The REJECT control for the expression spelling: a user exception whose
// constructor is NOT `pure` fails condition (a), and the diagnostic is the
// carve-out's own ("class construction at `throw`"), on the throw's line —
// the same diagnostic the statement spelling reports.
BEZEL_TEST(pure_body_stdlib_member_7453,
           throw_expression_carve_out_rejects_user_impure_ctor_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "public class MyError: Exception\n"
        "  public constructor(message: string)\n"
        "    base(message);\n"
        "  end constructor;\n"
        "end MyError;\n"
        "class C\n"
        "  public static pure method F(s: string?): string\n"
        "    return s ?? throw new MyError(\"null s\");\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "class construction at `throw` is not admitted in a pure body");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 9u);
    // The carve-out's report is the ONLY construction report: the plain
    // `NewExpr` arm is not reached for a throw operand.
    BEZEL_EXPECT_TRUE(diagnosticsContaining(r, "class construction is not")
                          .empty());
}

// The carve-out admits the CONSTRUCTION, not the arguments: an impure call
// inside the throw operand's argument list is still rule 6's business.
BEZEL_TEST(pure_body_stdlib_member_7453,
           throw_expression_operand_args_still_checked_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(s: string?): string\n"
        "    return s ?? throw new ArgumentException(Console.ReadLine() ?? "
        "\"x\");\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "the standard library does not declare `Console.ReadLine` pure");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 4u);
    BEZEL_EXPECT_TRUE(
        diagnosticsContaining(r, "standard-library class construction")
            .empty());
}

// §22.16: "`throw` is rejected (a clause states a proposition …)". The
// clause's throw is its ONE violation; its `new ArgumentException(…)`
// operand is still the carve-out's (admitted) business, not a second
// report. Pins the diagnostic count of conformance case s22_16_0006, which
// the earlier arm had raised to two.
BEZEL_TEST(pure_body_stdlib_member_7453,
           clause_throw_reports_the_throw_once_spec_22_16) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Ops\n"
        "  public static method Check(x: int): bool\n"
        "    requires(throw new ArgumentException(\"boom\"))\n"
        "    return true;\n"
        "  end Check;\n"
        "end Ops;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "`throw` is not admitted in a pure body");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 4u);
    BEZEL_EXPECT_TRUE(
        diagnosticsContaining(r, "class construction").empty());
    BEZEL_EXPECT_TRUE(r.diagnostics.diagnostics().size() == 1u);
}

// Review probe n01 — stdlib §2.15 `record JsonOptions(Indented: bool,
// IndentWidth: int, AllowTrailingCommas: bool, IgnoreUnknownFields: bool,
// MaxDepth: int)`. Spec §9.1: a positional record generates "Public
// read-only properties, one per positional field … auto-properties in
// §23.1's sense"; §22.15 *Members classified by rule*: "an auto-property
// getter is pure (a backing-field read)". All five component reads are
// admitted.
BEZEL_TEST(pure_body_stdlib_member_7453,
           json_options_component_reads_in_pure_body_accepted_spec_9_1_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "import System.Serialization;\n"
        "class C\n"
        "  public static pure method F(o: JsonOptions): int => o.MaxDepth + "
        "o.IndentWidth;\n"
        "  public static pure method G(o: JsonOptions): bool => o.Indented or "
        "o.AllowTrailingCommas or o.IgnoreUnknownFields;\n"
        "end C;\n");
    expectStdlibArmAdmits(r);
}

// The classification is by RECORD, so every built-in positional record's
// components are admitted: spec §31.4 `record ExceptionInfo(TypeName:
// string, Message: string, StackTrace: string)` (over-rejected by the
// earlier arm, which had transcribed only the stdlib's spans), stdlib §3.3
// `ReadOnlyStringSpan(Source, Start, Length)`, §4.8 `DictionaryEntry<K,
// V>(Key, Value)` and §4.15 `ArraySpan<T>` / `ReadOnlyArraySpan<T>(Source,
// Start, Length)`.
BEZEL_TEST(pure_body_stdlib_member_7453,
           positional_record_component_reads_accepted_spec_9_1_22_15_31_4) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method E(e: ExceptionInfo): string => e.TypeName "
        "+ e.Message + e.StackTrace;\n"
        "  public static pure method S(s: ReadOnlyStringSpan): int => s.Start "
        "+ s.Length;\n"
        "  public static pure method D(d: DictionaryEntry<string, int>): int "
        "=> d.Value + d.Key.Length;\n"
        "  public static pure method A(a: ArraySpan<int>): int => a.Start + "
        "a.Length;\n"
        "  public static pure method R(a: ReadOnlyArraySpan<int>): int => "
        "a.Start + a.Length;\n"
        "end C;\n");
    expectStdlibArmAdmits(r);
}

// The classification admits the COMPONENTS, not every member of a
// positional record: `JsonOptions` declares no `pure` member, so a call
// to one its header does not name is still rule 6's rejection.
BEZEL_TEST(pure_body_stdlib_member_7453,
           positional_record_non_component_member_still_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  public static pure method F(s: ReadOnlyStringSpan): bool => "
        "s.IsEmpty;\n"
        "  public static pure method G(sp: ReadOnlySpan<int>): bool => "
        "sp.IsEmpty;\n"
        "end C;\n");
    // `ReadOnlyStringSpan.IsEmpty` is `pure`-declared (stdlib §3.3);
    // `ReadOnlySpan<T>.IsEmpty` carries no marker (stdlib §4.14).
    const bezel::Diagnostic* d = theDiagnosticContaining(
        r, "the standard library does not declare `ReadOnlySpan.IsEmpty` "
           "pure");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 4u);
    BEZEL_EXPECT_TRUE(diagnosticsContaining(r, "ReadOnlyStringSpan.IsEmpty")
                          .empty());
}

// =========================================================================
// Behaviour flips the §22.15 stdlib arm brings to CONTRACT CLAUSES. §22.16:
// a clause "is admitted under the pure-body rules of §22.15, applied
// expression-wise", so a standard-library member the library does not
// mark `pure` is rejected in a clause exactly as in a pure body. These
// three shapes compiled before #7453 (review probes n29, n22, n21).
// =========================================================================
BEZEL_TEST(pure_body_stdlib_member_7453,
           clause_unmarked_stdlib_members_rejected_spec_22_16) {
    auto r = parseSemaAll(
        "module M;\n"
        "class C\n"
        "  private _sb: StringBuilder := new StringBuilder();\n"
        "  invariant(_sb.Length >= 0)\n"
        "  public method F(a: Array<int>, x: int): bool\n"
        "    requires(a.Contains(x))\n"
        "    return true;\n"
        "  end F;\n"
        "  public method G(): bool\n"
        "    requires(Console.ReadLine() <> null)\n"
        "    return true;\n"
        "  end G;\n"
        "end C;\n");
    // stdlib §3.1 declares `Length` with no marker (`Capacity` carries one).
    const bezel::Diagnostic* l = theDiagnosticContaining(
        r, "the standard library does not declare `StringBuilder.Length` "
           "pure");
    BEZEL_EXPECT_TRUE(l != nullptr);
    if (l) BEZEL_EXPECT_TRUE(l->location.line == 4u);
    // stdlib §4.4 declares `Contains` with no marker.
    const bezel::Diagnostic* c = theDiagnosticContaining(
        r, "the standard library does not declare `Array.Contains` pure");
    BEZEL_EXPECT_TRUE(c != nullptr);
    if (c) BEZEL_EXPECT_TRUE(c->location.line == 6u);
    // stdlib §2.1 declares `ReadLine` with no marker (an I/O intercept).
    const bezel::Diagnostic* rl = theDiagnosticContaining(
        r, "the standard library does not declare `Console.ReadLine` pure");
    BEZEL_EXPECT_TRUE(rl != nullptr);
    if (rl) BEZEL_EXPECT_TRUE(rl->location.line == 10u);
}
