// fix #3582 — `Outer<int>.Tag`: a §11.6 nested type reached through an
// EXPLICITLY-ARGUED enclosing generic. Spec §11.6 / Appendix A.5 / §2.7, all
// read at v0.286.
//
// WHAT THE DEFECT WAS. Two layers, one root.
//
//   1. THE CRASH (the one that matters). `bezelc` SEGV'd — exit 139, core
//      dumped, NO diagnostic — on the single-module program in
//      `ascription_form_is_rejected_...` below. Measured on the pre-fix tree
//      at fd39253; the stack was
//        parsePostfix (Parser_Exprs.cpp:1995) <- parseUnaryExpression <-
//        parseBinaryExpression <- parseExpressionImpl <- parseExpression <-
//        parseStatementImpl (Parser_Stmts.cpp:1029) <- ... <-
//        parseCompilationUnit
//      `parsePrimaryExpression` returns a NULL expression at exactly the two
//      points where it has already REPORTED a located error ("expected
//      expression"; §17.13's bare `_`), and every suffix arm in `parsePostfix`
//      reads `head->span.begin`. So any input that leaves a postfix-suffix
//      token where a primary expression must start dereferenced null. That is
//      a whole CLASS, not one program: a bare `?.` and a bare `!` in statement
//      position each SEGV'd the pre-fix compiler on their own (PART 1 below),
//      with no generics and no nested types anywhere in the source.
//
//   2. THE GAP that steered the crashing input there. `parseType` implements
//      A.5 exactly — `TypeNoNullable ::= QualifiedName GenericArgs?` over
//      `QualifiedName ::= Identifier ("." Identifier)*` — so the argument list
//      attaches to the END of the whole dotted name and `Outer<int>.Tag` is
//      NOT a Type. `parseType` simply stopped after `Outer<int>`, leaving the
//      `.Tag` to desynchronise the enclosing construct: in a `new` it fell out
//      as the unrelated "expected '(' to begin constructor argument list", and
//      in a local's type ascription it started a "statement" at `.`, which is
//      how the null head reached `parsePostfix`.
//
// WHICH CONCLUSION THE SPEC SUPPORTS. Rejection, with a remedy. A.5 (v0.286)
// admits no argument list on a non-final segment; §11.6 (v0.286) says "A
// nested type is referenced from outside as `Outer.Nested`"; and §2.7's
// leftmost-first chain resolves "the rest of the chain ... against that type
// (nested type, enum constant, static member — each segment in turn)", i.e.
// over plain identifiers. `Outer.Tag` is therefore the spelling that names the
// type, and it ALREADY WORKS for a nested type inside a generic enclosing type
// — PART 3 pins that (it returns 7), which is what makes the rejection a
// naming rule rather than a missing feature.
//
// Note `Box<int>.Five()` — a static member on a CONSTRUCTED generic — is an
// expression chain (§22.12's balanced scan), not a Type, and is untouched;
// PART 3 pins it too, as the over-rejection guard for the new diagnostic.

#include <string>

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// True iff the program is REJECTED and some diagnostic contains `needle`.
// Both halves matter: `hasErrors()` alone would pass on an unrelated failure,
// and a matching string on a program that still compiled would prove nothing.
// A compiler SEGV lands here as CompileOutcome::Crashed, whose synthesized
// diagnostic reads "runAot: compiler CRASHED (signal 11)" — it contains none
// of the needles below, so every row in PART 1 and PART 2 fails on the
// pre-fix compiler.
bool rejects(const std::string& prog, const std::string& needle) {
    auto r = runAot(prog);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// True iff SOME diagnostic mentions a compiler crash. The direct, positive
// statement of "the compiler did not fall over", independent of the message
// texts asserted elsewhere.
bool compilerCrashed(const std::string& prog) {
    auto r = runAot(prog);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("CRASHED") != std::string::npos) return true;
    return false;
}

std::string body(const std::string& stmts) {
    return "module M; public class C public static method Main(): int " +
           stmts + " end Main; end C;";
}

// §11.6: a nested class in a GENERIC enclosing class. `Tag` uses none of
// `Outer`'s type parameters, so `Outer.Tag` names it unambiguously.
const char* kOuter =
    "public class Outer<T> "
    "  public class Tag "
    "    private _n: int; "
    "    public constructor(n: int) _n := n; end constructor; "
    "    public method N(): int => _n; "
    "  end Tag; "
    "end Outer; ";

std::string withOuter(const std::string& stmts) {
    return "module M; " + std::string(kOuter) +
           "public class C public static method Main(): int " + stmts +
           " end Main; end C;";
}

}  // namespace

// =============================================================================
// PART 1 — the CRASH CLASS. A postfix-suffix token where a primary expression
// must start. Each of these SEGV'd the pre-fix compiler (exit 139) with no
// diagnostic at all; each must now produce the located error the primary
// parser already reported. No generics, no nested types — this is the guard
// that keeps the fix at the null-propagation level rather than at the one
// input that found it. Appendix C: a compiler crash is not an admissible
// outcome for any input.
// =============================================================================

BEZEL_TEST(nested_type_generic_enclosing_3582,
           bare_null_conditional_suffix_reports_not_crashes_spec_a5) {
    const std::string p = body("?. x; return 0;");
    BEZEL_EXPECT_FALSE(compilerCrashed(p));
    BEZEL_EXPECT_TRUE(rejects(p, "expected expression"));
}

BEZEL_TEST(nested_type_generic_enclosing_3582,
           bare_null_forgiving_suffix_reports_not_crashes_spec_a5) {
    const std::string p = body("!; return 0;");
    BEZEL_EXPECT_FALSE(compilerCrashed(p));
    BEZEL_EXPECT_TRUE(rejects(p, "expected expression"));
}

// =============================================================================
// PART 2 — the GAP, diagnosed. `Outer<int>.Tag` is rejected in BOTH positions
// with ONE message that names the rule and the remedy.
// =============================================================================

// The program from the issue, verbatim in shape: the type-ASCRIPTION form.
// This is the one that SEGV'd.
BEZEL_TEST(nested_type_generic_enclosing_3582,
           ascription_form_is_rejected_with_remedy_spec_11_6) {
    const std::string p = withOuter(
        "using var t: Outer<int>.Tag := new Outer<int>.Tag(7); return t.N();");
    BEZEL_EXPECT_FALSE(compilerCrashed(p));
    BEZEL_EXPECT_TRUE(
        rejects(p,
                "a generic argument list attaches to the end of a qualified "
                "type name"));
    BEZEL_EXPECT_TRUE(rejects(p, "write `Outer.Tag` to name the nested type"));
}

// The `new`-EXPRESSION form, which the issue recorded separately: pre-fix it
// was rejected, but with the unrelated "expected '(' to begin constructor
// argument list" — the parser had stopped at `Outer<int>` and blamed the `.`
// on a missing argument list. Both spellings must now name the same rule.
BEZEL_TEST(nested_type_generic_enclosing_3582,
           new_expression_form_is_rejected_with_remedy_spec_11_6) {
    const std::string p =
        withOuter("using var t := new Outer<int>.Tag(7); return t.N();");
    BEZEL_EXPECT_FALSE(compilerCrashed(p));
    BEZEL_EXPECT_TRUE(
        rejects(p,
                "a generic argument list attaches to the end of a qualified "
                "type name"));
    BEZEL_EXPECT_FALSE(
        rejects(p, "expected '(' to begin constructor argument list"));
}

// =============================================================================
// PART 3 — OVER-REJECTION GUARDS. Everything the new diagnostic must NOT
// touch, each asserted on a produced VALUE rather than on "it compiled".
// =============================================================================

// §11.6 "A nested type is referenced from outside as `Outer.Nested`" — and it
// works when the enclosing type is generic, which is what makes the PART 2
// rejection a spelling rule with a remedy rather than an unimplemented
// feature.
BEZEL_TEST(nested_type_generic_enclosing_3582,
           nested_type_of_generic_outer_runs_spec_11_6) {
    BEZEL_EXPECT_EQ(
        run(withOuter("using var t: Outer.Tag := new Outer.Tag(7); "
                      "return t.N();")),
        7);
}

// §22.12 static member on a CONSTRUCTED generic: an expression chain, parsed
// by the postfix path, never by `parseType`. The `.` after `<int>` here is
// legal and must stay legal.
BEZEL_TEST(nested_type_generic_enclosing_3582,
           static_member_on_constructed_generic_still_runs_spec_22_12) {
    BEZEL_EXPECT_EQ(
        run("module M; public class Box<T> "
            "  public static method Five(): int => 5; "
            "end Box; "
            "public class C public static method Main(): int "
            "  return Box<int>.Five(); "
            "end Main; end C;"),
        5);
}

// §14.1 an ordinary constructed generic in an ascription — a `<...>` that is
// NOT followed by `.` — is untouched.
BEZEL_TEST(nested_type_generic_enclosing_3582,
           plain_constructed_generic_ascription_still_runs_spec_14_1) {
    BEZEL_EXPECT_EQ(
        run("module M; public class Box<T> "
            "  private _v: int; "
            "  public constructor(v: int) _v := v; end constructor; "
            "  public method V(): int => _v; "
            "end Box; "
            "public class C public static method Main(): int "
            "  using var b: Box<int> := new Box<int>(9); "
            "  return b.V(); "
            "end Main; end C;"),
        9);
}
