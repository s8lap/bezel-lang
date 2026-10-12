// Spec-derived parser tests for the §A.4 Argument grammar:
//
//   ArgList       ::= Argument ("," Argument)*
//   Argument      ::= ArgModifiers? (Identifier ":=")? ArgExpression
//   ArgModifiers  ::= "ref" | "out" "var"? | "out" "using" "var"
//                   | "out" "owned" "var"
//                   // Note: bare "owned" was removed per §A.4 /
//                   // proposal 0090 (v0.246); use `move x` to transfer.
//   ArgExpression ::= Expression | "_"
//
// v0.160 (§22.4/§22.5): the former bare `out owned x` form now requires "var"
// (inline declaration); named form is positional-only for modified args.
//
// Slice 49 extends call/new/null-cond-call argument lists with modifiers,
// named arguments, and the `_` discard form. Earlier slices treated all
// arguments as plain expressions.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::exprToString;
using bezel::test::parseAll;

namespace {

const bezel::ast::Expr* extract(const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return nullptr;
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return nullptr;
    const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
        md->stmts[0].get());
    if (!ds) return nullptr;
    return ds->value.get();
}

std::string render(const std::string& expr) {
    const std::string src = "module M; class C public method M() _ := " +
                            expr + "; end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    const auto* e = extract(r);
    return e ? exprToString(*e) : std::string("<no-expr>");
}

} // namespace

BEZEL_TEST(parser_arg, ref_modifier) {
    BEZEL_EXPECT_EQ(render("f(ref x)"), std::string("f(ref x)"));
}

BEZEL_TEST(parser_arg, out_modifier) {
    BEZEL_EXPECT_EQ(render("f(out x)"), std::string("f(out x)"));
}

BEZEL_TEST(parser_arg, out_var_modifier) {
    BEZEL_EXPECT_EQ(render("f(out var x)"), std::string("f(out var x)"));
}

BEZEL_TEST(parser_arg, out_using_var_modifier) {
    BEZEL_EXPECT_EQ(
        render("f(out using var r)"),
        std::string("f(out using var r)"));
}

// v0.160: `out owned var` is the inline-declaration form (§22.4).
BEZEL_TEST(parser_arg, out_owned_var_modifier) {
    BEZEL_EXPECT_EQ(
        render("f(out var r)"),
        std::string("f(out var r)"));
}

// per §A.4 / proposal 0090 (v0.246): bare `owned` is not an ArgModifiers
// alternative; `M(owned x)` is a parse error. Use `move x` (§22.3, §28.8.1).
BEZEL_TEST(parser_arg, owned_modifier) {
    BEZEL_EXPECT_EQ(render("Take(owned resource)"),
                    std::string("<errors>"));
}

BEZEL_TEST(parser_arg, named_arg) {
    BEZEL_EXPECT_EQ(
        render("f(name := \"x\", age := 30)"),
        std::string("f(name := \"x\", age := 30)"));
}

// v0.160 §22.5: `ref`/`out`/`owned` arguments are positional-only;
// combining a modifier with the named `Identifier :=` form is a parse error.
BEZEL_TEST(parser_arg, modifier_and_named_is_error) {
    // `ref x := source` — modifier present, named form invalid.
    BEZEL_EXPECT_EQ(render("f(ref x := source)"), std::string("<errors>"));
}

BEZEL_TEST(parser_arg, discard_underscore) {
    BEZEL_EXPECT_EQ(
        render("f(out _, out var keep)"),
        std::string("f(out _, out var keep)"));
}

// per §A.4 / proposal 0090 (v0.246): bare `owned` is not an ArgModifiers
// alternative; the `owned r` argument is a parse error. Use `move r`.
BEZEL_TEST(parser_arg, mixed_args) {
    BEZEL_EXPECT_EQ(
        render(
            "f(1, ref x, owned r, name := \"x\", out _, out using var d)"),
        std::string("<errors>"));
}

// per §A.4 / proposal 0090 (v0.246): bare `owned` is not an ArgModifiers
// alternative; `new Foo(ref x, owned r)` is a parse error. Use `move r`.
BEZEL_TEST(parser_arg, new_with_modifiers) {
    BEZEL_EXPECT_EQ(render("new Foo(ref x, owned r)"),
                    std::string("<errors>"));
}

BEZEL_TEST(parser_arg, null_cond_call_with_named) {
    BEZEL_EXPECT_EQ(
        render("obj?(name := \"x\")"),
        std::string("obj?(name := \"x\")"));
}

BEZEL_TEST(parser_arg, plain_args_still_work) {
    // Regression: ordinary argument shapes without modifiers/names continue
    // to render unchanged.
    BEZEL_EXPECT_EQ(
        render("f(a + b, c * d, g(e))"),
        std::string("f((a + b), (c * d), g(e))"));
}
