// fix #7163 — §22.12's generics-vs-less-than resolution named EIGHT tokens
// after the closing `>`, and the compiler implemented TWO scan helpers that
// each covered a DIFFERENT SUBSET of that set: the "method" scan admitted
// `(`, `)`, `]`, `,`, `;` and the "receiver" scan admitted only `.`.  Between
// them they omitted `?.` and `[` EVERYWHERE, so `Box<int>?.Make()` and
// `Box<int>[0]` failed to parse (or parsed relationally).
//
// THE RULE, verbatim (§22.12, v0.343):
//
//   "After an identifier in expression position, a `<` opens generic arguments
//    **iff** scanning forward finds a balanced `<…>` group (respecting nesting)
//    whose closing `>` is immediately followed by a token that can continue a
//    primary expression — `.`, `?.`, `(`, or `[` — or by a token that cannot
//    begin an operand — `)`, `]`, `,`, or `;` (a relational `>` always has a
//    right operand, so no relational reading is lost; this is how a method
//    reference at explicit type arguments, `xs.Select(Util.Id<int>)` or
//    `var k := Util.Keep<Pair>;`, parses as one, §27.6).  Otherwise the `<` is
//    the relational operator."
//
// The successor set is therefore EIGHT tokens, and it is NORMATIVE and
// PURELY SYNTACTIC — the same rule at every commit point (A.7's
// `PrimaryAtom ::= … | Identifier GenericArgs?` and
// `PrimarySuffix ::= "." Identifier GenericArgs? | "?." Identifier
// GenericArgs?`).
//
// THE FIX: ONE scan (Parser::tokensLookLikeGenericArgs) applies that set, and
// every commit point consults it, so the set cannot drift per site again.
//
// THE ORACLE: the parse-only oracle (parseAll) for the rows whose property is
// WHICH READING THE PARSE CHOSE — the shared renderer prints names but not the
// type-argument lists, so this file walks the chain itself; runAot (the real
// AOT path) for the rows that must still run.
//
// THE RELATIONAL READING IS NOT LOST. §22.12's rule is normative as written:
// a closing `>` followed by one of the eight successors commits the GENERIC
// reading.  PART 3 pins the relational spellings that keep their relational
// parse, and PART 4 pins the spellings the generic reading newly captures —
// including the spec's own example `f(a<b, c>(d))`, which §22.12 says to
// parenthesize to express the relational reading.

#include <memory>
#include <string>
#include <vector>

#include "ast/Node.hpp"
#include "support/aot_support.hpp"
#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;
using bezel::test::runAot;
using bezel::test::typeToString;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// The initializer expression of `public x: int := EXPR;`, or null.
const bezel::ast::Expr* initExpr(const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return nullptr;
    const auto* fd = dynamic_cast<const bezel::ast::FieldDecl*>(
        td->members[0].get());
    if (!fd) return nullptr;
    return fd->initializer.get();
}

// The explicit type arguments §22.12's scan COMMITS, in source order, each
// tagged with the AST node that carries them — the chain head (an
// IdentifierExpr, A.7's PrimaryAtom slot), a dotted member (a
// MemberAccessExpr, the "." PrimarySuffix slot) or a null-conditional member
// (a NullCondMemberExpr, the "?." PrimarySuffix slot).  The shared renderer
// (parse_support.hpp) prints the names but NOT the type-argument lists, so
// this walks the chain itself.
//
// A chain-head commit and a member commit are different shapes, and the site
// that reads the list is different too, so the tag is part of the assertion.
std::vector<std::string> committedTypeArgs(const bezel::ast::Expr* e) {
    std::vector<std::string> out;
    if (!e) return out;
    auto add = [&](const char* kind,
                   const std::vector<std::unique_ptr<bezel::ast::Type>>& ts) {
        if (ts.empty()) return;
        std::string s = kind;
        s += ":";
        s += "<";
        for (std::size_t i = 0; i < ts.size(); ++i) {
            if (i) s += ", ";
            s += ts[i] ? typeToString(*ts[i]) : std::string("<null>");
        }
        s += ">";
        out.push_back(s);
    };
    if (auto* id = dynamic_cast<const bezel::ast::IdentifierExpr*>(e)) {
        add("head", id->typeArgs);
        return out;
    }
    if (auto* m = dynamic_cast<const bezel::ast::MemberAccessExpr*>(e)) {
        auto sub = committedTypeArgs(m->target.get());
        out.insert(out.end(), sub.begin(), sub.end());
        add("member", m->typeArgs);
        return out;
    }
    if (auto* n = dynamic_cast<const bezel::ast::NullCondMemberExpr*>(e)) {
        auto sub = committedTypeArgs(n->target.get());
        out.insert(out.end(), sub.begin(), sub.end());
        add("nullcond", n->typeArgs);
        return out;
    }
    if (auto* c = dynamic_cast<const bezel::ast::CallExpr*>(e)) {
        out = committedTypeArgs(c->callee.get());
        for (const auto& a : c->args) {
            auto sub = committedTypeArgs(a.value.get());
            out.insert(out.end(), sub.begin(), sub.end());
        }
        return out;
    }
    if (auto* c = dynamic_cast<const bezel::ast::NullCondCallExpr*>(e)) {
        out = committedTypeArgs(c->callee.get());
        for (const auto& a : c->args) {
            auto sub = committedTypeArgs(a.value.get());
            out.insert(out.end(), sub.begin(), sub.end());
        }
        return out;
    }
    if (auto* i = dynamic_cast<const bezel::ast::IndexExpr*>(e)) {
        out = committedTypeArgs(i->target.get());
        for (const auto& a : i->indices) {
            auto sub = committedTypeArgs(a.value.get());
            out.insert(out.end(), sub.begin(), sub.end());
        }
        return out;
    }
    if (auto* p = dynamic_cast<const bezel::ast::PostfixBangExpr*>(e))
        return committedTypeArgs(p->operand.get());
    if (auto* b = dynamic_cast<const bezel::ast::BinaryExpr*>(e)) {
        auto l = committedTypeArgs(b->left.get());
        auto r = committedTypeArgs(b->right.get());
        l.insert(l.end(), r.begin(), r.end());
        return l;
    }
    if (auto* u = dynamic_cast<const bezel::ast::UnaryExpr*>(e))
        return committedTypeArgs(u->operand.get());
    return out;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ";";
        s += v[i];
    }
    return s;
}

// Parse `public x: int := EXPR;` and return the committed type arguments, as
// one string.  "<parse-error>" when the program does not parse at all — which
// is itself the pre-fix failure for the shapes the fix repairs.
std::string committed(const std::string& expr) {
    auto r = parseAll(std::string("module M; class Foo public x: int := ") +
                      expr + std::string("; end Foo;"));
    if (r.diagnostics.hasErrors()) return "<parse-error>";
    return join(committedTypeArgs(initExpr(r)));
}

// Parse `public x: int := EXPR;` and return the initializer's rendered text.
// The renderer wraps binary expressions in explicit parentheses, so precedence
// and associativity are directly observable.
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

}  // namespace

// =============================================================================
// PART 1 — THE COORDINATOR'S FOUR MEASURED SHAPES.  Each parsed RELATIONALLY
// (or not at all) before the fix; each must now take the generic reading, on
// the AST node its site's slot names.
// =============================================================================

// `Box<int>?.Make()` — the `?.` successor at the chain head.  Pre-fix: "expected
// expression" (the `?.` was honoured nowhere).
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_chain_head_commits_on_the_null_conditional_successor) {
    BEZEL_EXPECT_EQ(committed("Box<int>?.Make()"), std::string("head:<int>"));
}

// `Box<int>[0]` — the `[` successor at the chain head.  Pre-fix: parsed
// RELATIONALLY ("operator `<` is not defined for type `Box`").
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_chain_head_commits_on_the_index_successor) {
    BEZEL_EXPECT_EQ(committed("Box<int>[0]"), std::string("head:<int>"));
}

// `b.Plain<int>?.ToString()` — the `?.` successor at a dotted member.
// Pre-fix: "expected expression".
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_dotted_member_commits_on_the_null_conditional_successor) {
    BEZEL_EXPECT_EQ(committed("b.Plain<int>?.ToString()"),
                    std::string("member:<int>"));
}

// `b.Plain<int>[0]` — the `[` successor at a dotted member.  Pre-fix: parsed
// RELATIONALLY ("operator `>` is not defined for `bool` operands").
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_dotted_member_commits_on_the_index_successor) {
    BEZEL_EXPECT_EQ(committed("b.Plain<int>[0]"),
                    std::string("member:<int>"));
}

// The `?.` successor at a NULL-CONDITIONAL member — the third commit point.
// Pre-fix: "expected expression" (the `?.` was honoured nowhere there either).
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_null_conditional_member_commits_on_the_null_conditional_successor) {
    BEZEL_EXPECT_EQ(committed("h?.Id<int>?.Get()"),
                    std::string("nullcond:<int>"));
}

// The `[` successor at a NULL-CONDITIONAL member.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_null_conditional_member_commits_on_the_index_successor) {
    BEZEL_EXPECT_EQ(committed("h?.Id<int>[0]"),
                    std::string("nullcond:<int>"));
}

// The `.` successor at a NULL-CONDITIONAL member — honoured at no site before
// the fix (the receiver scan's `.` was consulted only at the chain head and
// at a dotted segment).
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_null_conditional_member_commits_on_the_dot_successor) {
    BEZEL_EXPECT_EQ(committed("h?.Id<int>.Get()"),
                    std::string("nullcond:<int>"));
}

// =============================================================================
// PART 2 — ALL EIGHT SUCCESSORS, AT EVERY COMMIT POINT.  §22.12's rule is
// ONE rule: the same eight tokens at the chain head, at a dotted member and
// at a null-conditional member.
// =============================================================================

// The chain head (A.7's PrimaryAtom slot): `.`, `?.`, `(`, `[`, `)`, `]`,
// `,`, `;`.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_chain_head_commits_on_every_successor) {
    BEZEL_EXPECT_EQ(committed("Box<int>.Make()"), std::string("head:<int>"));
    BEZEL_EXPECT_EQ(committed("Box<int>?.Make()"), std::string("head:<int>"));
    BEZEL_EXPECT_EQ(committed("TopId<int>(3)"), std::string("head:<int>"));
    BEZEL_EXPECT_EQ(committed("Box<int>[0]"), std::string("head:<int>"));
    BEZEL_EXPECT_EQ(committed("Mk1(Box<int>)"), std::string("head:<int>"));
    BEZEL_EXPECT_EQ(committed("Arr[Box<int>]"), std::string("head:<int>"));
    BEZEL_EXPECT_EQ(committed("Mk2(Box<int>, 1)"), std::string("head:<int>"));
    BEZEL_EXPECT_EQ(committed("Box<int>"), std::string("head:<int>"));
}

// A dotted member (A.7's "." PrimarySuffix slot): the same eight.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_dotted_member_commits_on_every_successor) {
    BEZEL_EXPECT_EQ(committed("h.Id<int>.Get()"), std::string("member:<int>"));
    BEZEL_EXPECT_EQ(committed("h.Id<int>?.Get()"), std::string("member:<int>"));
    BEZEL_EXPECT_EQ(committed("h.Id<int>(3)"), std::string("member:<int>"));
    BEZEL_EXPECT_EQ(committed("h.Id<int>[0]"), std::string("member:<int>"));
    BEZEL_EXPECT_EQ(committed("Mk1(h.Id<int>)"), std::string("member:<int>"));
    BEZEL_EXPECT_EQ(committed("Arr[h.Id<int>]"), std::string("member:<int>"));
    BEZEL_EXPECT_EQ(committed("Mk2(h.Id<int>, 1)"), std::string("member:<int>"));
    BEZEL_EXPECT_EQ(committed("h.Id<int>"), std::string("member:<int>"));
}

// A null-conditional member (A.7's "?." PrimarySuffix slot): the same eight.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_null_conditional_member_commits_on_every_successor) {
    BEZEL_EXPECT_EQ(committed("h?.Id<int>.Get()"),
                    std::string("nullcond:<int>"));
    BEZEL_EXPECT_EQ(committed("h?.Id<int>?.Get()"),
                    std::string("nullcond:<int>"));
    BEZEL_EXPECT_EQ(committed("h?.Id<int>(3)"), std::string("nullcond:<int>"));
    BEZEL_EXPECT_EQ(committed("h?.Id<int>[0]"), std::string("nullcond:<int>"));
    BEZEL_EXPECT_EQ(committed("Mk1(h?.Id<int>)"), std::string("nullcond:<int>"));
    BEZEL_EXPECT_EQ(committed("Arr[h?.Id<int>]"),
                    std::string("nullcond:<int>"));
    BEZEL_EXPECT_EQ(committed("Mk2(h?.Id<int>, 1)"),
                    std::string("nullcond:<int>"));
    BEZEL_EXPECT_EQ(committed("h?.Id<int>"), std::string("nullcond:<int>"));
}

// =============================================================================
// PART 3 — THE RELATIONAL READING IS NOT LOST.  A closing `>` followed by
// anything OUTSIDE the eight successors leaves the `<` to the relational
// operator, and every relational spelling keeps its relational parse.
// =============================================================================

BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_relational_comparison_stays_relational) {
    BEZEL_EXPECT_EQ(parseInitText("a < b"), std::string("(a < b)"));
}

BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_relational_chain_stays_relational) {
    BEZEL_EXPECT_EQ(parseInitText("a < b > c"), std::string("((a < b) > c)"));
}

BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_relational_with_an_indexed_operand_stays_relational) {
    BEZEL_EXPECT_EQ(parseInitText("x[i] < y"), std::string("(x[i] < y)"));
}

BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_relational_with_call_operands_stays_relational) {
    BEZEL_EXPECT_EQ(parseInitText("f(a, b) < g(c, d)"),
                    std::string("(f(a, b) < g(c, d))"));
}

// A relational chain at a DOTTED segment: the `>` is followed by an operand,
// not by one of the eight successors.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_relational_after_a_dotted_segment_stays_relational) {
    BEZEL_EXPECT_EQ(parseInitText("a.b < c"), std::string("(a.b < c)"));
    BEZEL_EXPECT_EQ(parseInitText("a.b < c > d"),
                    std::string("((a.b < c) > d)"));
}

// No type arguments are committed by a relational spelling: the generic
// reading did NOT win.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_relational_spelling_commits_no_type_arguments) {
    BEZEL_EXPECT_EQ(committed("a < b"), std::string(""));
    BEZEL_EXPECT_EQ(committed("a < b > c"), std::string(""));
    BEZEL_EXPECT_EQ(committed("x[i] < y"), std::string(""));
    BEZEL_EXPECT_EQ(committed("f(a, b) < g(c, d)"), std::string(""));
}

// =============================================================================
// PART 4 — WHAT THE GENERIC READING CAPTURES.  §22.12's rule is normative as
// written: the closing `>` followed by one of the eight successors commits
// the GENERIC reading.  The spec says so of `f(a<b, c>(d))` — "to express the
// relational reading, parenthesize" — and the `[` successor does the same to
// `a < b > [c]`.
// =============================================================================

// The spec's own consequence, stated at §22.12: `f(a<b, c>(d))` parses as a
// generic call, never as two relational arguments.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_generic_call_wins_over_the_relational_reading) {
    BEZEL_EXPECT_EQ(committed("f(a<b, c>(d))"), std::string("head:<b, c>"));
}

// The same for the `[` successor: `a < b > [c]` is a generic reading whose
// type argument is `b` and whose suffix is the index.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_index_successor_wins_over_the_relational_reading) {
    BEZEL_EXPECT_EQ(committed("a < b > [c]"), std::string("head:<b>"));
}

// The `?.` successor: `a < b > ?.c` is a generic reading too.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_null_conditional_successor_wins_over_the_relational_reading) {
    BEZEL_EXPECT_EQ(committed("a < b > ?.c"), std::string("head:<b>"));
}

// =============================================================================
// PART 5 — THE SPEC'S OWN WORKED EXAMPLES STILL RUN.  The rule the fix
// implements is the same rule those examples were written against.
// =============================================================================

// §11.6's worked example: `Tree<int>.Node<int>.Create()` — the chain head
// supplies the enclosing type's arguments, the dotted segment the nested
// type's own.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_11_6_nested_type_worked_example_still_runs) {
    auto r = runAot(
        "module M; "
        "public class Tree<T> "
        "  public class Node<U> "
        "    public static method Create(): int "
        "      return 42; "
        "    end Create; "
        "  end Node; "
        "end Tree; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Tree<int>.Node<int>.Create()); "
        "  return 0; "
        "end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// The `>>` closing-angle case: the enclosing type's own argument list closes
// with `>>` (the lexer emits it as one token).
BEZEL_TEST(generic_args_successor_set_7163,
           spec_11_6_nested_generics_close_angle_bracket_case_still_runs) {
    auto r = runAot(
        "module M; "
        "public class Tree<T> "
        "  public class Node<U> "
        "    public static method Create(): int "
        "      return 42; "
        "    end Create; "
        "  end Node; "
        "end Tree; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Tree<Array<int>>.Node<int>.Create()); "
        "  return 0; "
        "end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// §27.6's first spelling: `xs.Select(Util.Id<int>)` — a method reference at
// explicit type arguments, whose closing `>` is followed by `)`.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_27_6_method_reference_at_explicit_type_arguments_still_runs) {
    auto r = runAot(
        "module M; "
        "public class Util "
        "  public static method Id<T>(v: T): T "
        "    return v; "
        "  end Id; "
        "end Util; "
        "public class D public static method Main(): int "
        "  var xs: Array<int> := [1, 2]; "
        "  var ys := xs.Select(Util.Id<int>); "
        "  return 0; "
        "end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.6's second spelling: `var k := Util.Keep<Pair>;` — a method reference
// formed at explicit type arguments, whose closing `>` is followed by `;`.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_27_6_method_reference_formed_at_explicit_type_arguments_still_runs) {
    auto r = runAot(
        "module M; "
        "public class Pair "
        "  public A: int; "
        "  public constructor() A := 1; end constructor; "
        "end Pair; "
        "public class Util "
        "  public static method Keep<T>(): int "
        "    return 1; "
        "  end Keep; "
        "end Util; "
        "public class D public static method Main(): int "
        "  var k := Util.Keep<Pair>; "
        "  Console.WriteLine(k()); "
        "  return 0; "
        "end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// The chain-head receiver form: `Registry<int>.Make(7)`.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_22_12_chain_head_receiver_form_still_runs) {
    auto r = runAot(
        "module M; "
        "public class Registry<T> "
        "  public static method Make(): int "
        "    return 7; "
        "  end Make; "
        "end Registry; "
        "public class D public static method Main(): int "
        "  Console.WriteLine(Registry<int>.Make()); "
        "  return 0; "
        "end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// The bare-callee form: `Make<int>(7)` — §14.1's `Id<string>("hello")` shape.
BEZEL_TEST(generic_args_successor_set_7163,
           spec_14_1_bare_callee_explicit_type_arguments_still_runs) {
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
