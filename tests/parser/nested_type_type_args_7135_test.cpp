// fix #7135 — the spec's OWN §11.6 worked example did not parse:
// `Tree<int>.Node<int>.Create()`.
//
// THE DEFECT (measured on the pre-fix tree, at the lane's base SHA).
//
//   §11.6 (v0.343), "A nested type is one type, whatever the instantiation of
//   its enclosing type", names this exact program:
//
//     "A chain head that supplies the enclosing type's arguments (§2.7 rule 1,
//      `Tree<int>.Node<int>.Create()`) reaches that one type whatever the
//      arguments"
//
//   The compiler rejected it with "expected expression" (and a cascade of
//   follow-on diagnostics): the `<` after the DOTTED segment `Node` was left
//   for the relational-operator parser, so the program parsed as
//   `Tree<int>.Node < int > .Create()`.
//
// WHERE IT LIVED. §22.12's balanced scan (the "generics-vs-less-than"
// resolution) commits a generic-argument group at a CHAIN HEAD and at an
// EXPLICIT-TYPE-ARGUMENT CALLEE — but there was no commit point after a
// DOTTED segment, so a nested type's own type arguments had no parse. A.7's
// grammar gives the dotted segment the slot (`PrimarySuffix ::= "." Identifier
// GenericArgs?`) and points it at §22.12, whose successor rule is the union of
// the two scans the compiler already had:
//
//   "After an identifier in expression position, a `<` opens generic arguments
//    iff scanning forward finds a balanced `<…>` group (respecting nesting)
//    whose closing `>` is immediately followed by a token that can continue a
//    primary expression — `.`, `?.`, `(`, or `[` — or by a token that cannot
//    begin an operand — `)`, `]`, `,`, or `;`"
//
//   ⇒ `.` ⇒ a nested type's type arguments (§11.6's `Tree<int>.Node<int>`);
//     `(` ⇒ a method's explicit type arguments (§14.1 / §27.6).
//
// THE RELATIONAL READING IS NOT AT RISK. §22.12's own words: "a relational `>`
// always has a right operand". A `.` cannot begin an operand, so no SUCCESSFUL
// relational parse ever has a closing `>` followed by `.`; committing the
// generic reading there can only turn a program that FAILED to parse into one
// that parses. Every relational spelling whose closing `>` is followed by
// anything else keeps the relational parse — PART 3 pins them.
//
// ORACLE: runAot (the real AOT path) for the end-to-end rows; the parse-only
// oracle (parseAll) for the rows whose property is the SHAPE of the parse.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;
using bezel::test::runAot;

namespace {

// True iff the program is REJECTED and some diagnostic contains `needle`.
// Both halves matter: `hasErrors()` alone would pass on an unrelated failure,
// and a matching string on a program that still compiled would prove nothing.
bool rejects(const std::string& prog, const std::string& needle) {
    auto r = runAot(prog);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// Parse `public x: int := EXPR;` and return the initializer's rendered text.
// The renderer wraps binary expressions in explicit parentheses, so precedence
// and associativity are directly observable in the assertions below.
std::string parseInitText(const std::string& expr) {
    auto r = parseAll(std::string("module M; class Foo public x: int := ") +
                      expr + std::string("; end Foo;"));
    if (r.diagnostics.hasErrors()) return "<error>";
    const auto ms = memberSummaries(r);
    if (ms.size() != 1) return "<no member>";
    const std::string prefix = "public x: int := ";
    if (ms[0].rfind(prefix, 0) != 0) return "<bad prefix>";
    return ms[0].substr(prefix.size());
}

// §11.6's nested type: a generic class `Tree<T>` holding a generic nested
// class `Node<U>`, whose static member `Create` returns 42 and whose static
// field `Value` has the nested type's own type parameter as its type.
std::string withTree(const std::string& stmts) {
    return "module M; "
           "public class Tree<T> "
           "  public class Node<U> "
           "    public static readonly Value: U := default(U); "
           "    public static method Create(): int "
           "      return 42; "
           "    end Create; "
           "  end Node; "
           "end Tree; "
           "public class D public static method Main(): int " +
           stmts + " return 0; end Main; end D; ";
}

}  // namespace

// =============================================================================
// PART 1 — THE SPEC'S OWN WORKED EXAMPLE. §11.6 names this program; it must
// parse, and the nested type's own type arguments must be the ones applied.
// =============================================================================

// The literal example: the chain head `Tree<int>` supplies the ENCLOSING
// type's arguments and the dotted segment `Node<int>` supplies the NESTED
// type's own. Pre-fix: "expected expression" at the `<` of `Node<int>`.
BEZEL_TEST(nested_type_type_args_7135,
           spec_11_6_chain_head_with_nested_type_type_arguments_parses_and_runs) {
    auto r = runAot(withTree(
        "Console.WriteLine(Tree<int>.Node<int>.Create()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// The nested type's type arguments are APPLIED, not merely accepted: the
// static field's declared type is the nested type's own parameter `U`, so the
// two ascriptions below only typecheck when `U` is `int` in the first and
// `string` in the second. A parser that dropped the arguments would leave `U`
// unsubstituted and BOTH ascriptions would fail.
BEZEL_TEST(nested_type_type_args_7135,
           spec_11_6_nested_type_type_arguments_are_applied_to_the_nested_type) {
    auto r = runAot(withTree(
        "var i: int := Tree<int>.Node<int>.Value; "
        "var s: string := Tree<int>.Node<string>.Value; "
        "Console.WriteLine(i); "
        "Console.WriteLine(s); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n\n");
}

// The `>>` closing-angle case: the ENCLOSING type's own argument list closes
// with `>>` (`Array<int>` inside `Tree<…>`), which the lexer emits as one
// token. The nested type's own argument list is a separate group.
BEZEL_TEST(nested_type_type_args_7135,
           spec_11_6_nested_generics_close_angle_bracket_case) {
    auto r = runAot(withTree(
        "Console.WriteLine(Tree<Array<int>>.Node<int>.Create()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// =============================================================================
// PART 2 — THE NEIGHBOURING SPELLINGS. §22.12's scan commits a
// generic-argument group at a chain head and at an explicit-type-argument
// callee as well; those commit points are untouched and must keep working.
// =============================================================================

// The chain head: `Tree<int>.Make()` — the receiver form §22.12 names.
BEZEL_TEST(nested_type_type_args_7135,
           spec_22_12_chain_head_type_arguments_still_parse) {
    auto r = runAot(
        "module M; "
        "public class Tree<T> "
        "  public static method Make(): int "
        "    return 7; "
        "  end Make; "
        "end Tree; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Tree<int>.Make()); "
        "  return 0; "
        "end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// The callee form: `Box.Make<int>(7)` — explicit METHOD type arguments at a
// call site (§14.1 / §27.6), on a chain-head segment.
BEZEL_TEST(nested_type_type_args_7135,
           spec_14_1_explicit_method_type_arguments_on_a_chain_head_still_parse) {
    auto r = runAot(
        "module M; "
        "public class Box "
        "  public static method Make<T>(v: T): T "
        "    return v; "
        "  end Make; "
        "end Box; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Box.Make<int>(7)); "
        "  return 0; "
        "end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// A generic METHOD call after a DOTTED segment: `o.Id<string>("s")`. The
// closing `>` is followed by `(`, so the method reading is the one that
// commits — unchanged by this fix.
BEZEL_TEST(nested_type_type_args_7135,
           spec_14_1_generic_method_call_after_a_dotted_segment_still_parses) {
    auto r = runAot(
        "module M; "
        "public class Box "
        "  public method Id<T>(v: T): T "
        "    return v; "
        "  end Id; "
        "end Box; "
        "public class D public static method Main(): int "
        "  var o := new Box(); "
        "  Console.WriteLine(o.Id<string>(\"s\")); "
        "  return 0; "
        "end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "s\n");
}

// A bare callee with explicit type arguments: `Make<int>(7)` (§14.1's
// `Id<string>("hello")` shape).
BEZEL_TEST(nested_type_type_args_7135,
           spec_14_1_bare_callee_explicit_type_arguments_still_parse) {
    auto r = runAot(
        "module M; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Make<int>(7)); "
        "  return 0; "
        "end Main; "
        "public static method Make<T>(v: T): T "
        "  return v; "
        "end Make; "
        "end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// =============================================================================
// PART 3 — THE RELATIONAL READING IS NOT AT RISK. §22.12's scan leaves the `<`
// to the relational operator whenever the closing `>` is NOT followed by a
// token that can continue a primary expression. These are the shapes the fix
// must not touch; each is asserted on the SHAPE of the parse.
// =============================================================================

BEZEL_TEST(nested_type_type_args_7135,
           spec_22_12_relational_after_a_dotted_segment_stays_relational) {
    BEZEL_EXPECT_EQ(parseInitText("a.b < c"), std::string("(a.b < c)"));
}

BEZEL_TEST(nested_type_type_args_7135,
           spec_22_12_relational_chain_after_a_dotted_segment_stays_relational) {
    BEZEL_EXPECT_EQ(parseInitText("a.b < c > d"),
                    std::string("((a.b < c) > d)"));
}

BEZEL_TEST(nested_type_type_args_7135,
           spec_22_12_relational_with_an_indexed_operand_stays_relational) {
    BEZEL_EXPECT_EQ(parseInitText("x[i] < y"), std::string("(x[i] < y)"));
}

BEZEL_TEST(nested_type_type_args_7135,
           spec_22_12_relational_with_call_operands_stays_relational) {
    BEZEL_EXPECT_EQ(parseInitText("f(a, b) < g(c, d)"),
                    std::string("(f(a, b) < g(c, d))"));
}

// =============================================================================
// PART 4 — NO OVER-ACCEPTANCE. A type-argument list after a dotted segment is
// a spelling of exactly two things: a generic METHOD's explicit type
// arguments (§14.1 / §27.6) and a generic NESTED TYPE's type arguments
// (§11.6). §22.12 states the rule for every other name: "a name that resolves
// to a non-generic entity after a syntactically-successful generic reading is
// an ordinary error at that name".
// =============================================================================

// A member that is neither: the list spells nothing there, so it is an error
// at that name rather than a silently-ignored list.
BEZEL_TEST(nested_type_type_args_7135,
           spec_22_12_type_arguments_on_a_non_generic_member_are_an_error) {
    const std::string p =
        "module M; "
        "public class Box "
        "  public F: string; "
        "  public constructor() F := \"xy\"; end constructor; "
        "end Box; "
        "public class D public static method Main(): int "
        "  var o := new Box(); "
        "  Console.WriteLine(o.F<int>.Length); "
        "  return 0; "
        "end Main; end D; ";
    BEZEL_EXPECT_TRUE(rejects(p, "names no type parameters"));
}

// The same member WITHOUT a following segment: the list is still not a
// spelling of anything, so it is the same error at the same name.
BEZEL_TEST(nested_type_type_args_7135,
           spec_22_12_type_arguments_on_a_non_generic_member_are_an_error_alone) {
    const std::string p =
        "module M; "
        "public class Box "
        "  public F: int; "
        "  public constructor() F := 1; end constructor; "
        "end Box; "
        "public class D public static method Main(): int "
        "  var o := new Box(); "
        "  Console.WriteLine(o.F<int>); "
        "  return 0; "
        "end Main; end D; ";
    BEZEL_EXPECT_TRUE(rejects(p, "names no type parameters"));
}

// A nested type's type-argument list must fix its arity: `Node` declares one
// type parameter, so two arguments are the arity error (§14.1 / Appendix C.1).
BEZEL_TEST(nested_type_type_args_7135,
           spec_14_1_nested_type_type_arguments_must_fix_the_arity) {
    const std::string p = withTree(
        "Console.WriteLine(Tree<int>.Node<int, int>.Create()); ");
    BEZEL_EXPECT_TRUE(
        rejects(p,
                "generic-argument count does not match type's "
                "generic-parameter count"));
}
