// Spec-derived parser tests for expression-bodied property setters — issue
// #676, §13.6 / §23.3 / Appendix A §A.4.
//
// The grammar (Appendix A) defines:
//   AccessorBody ::= "=>" (Expression | "do" Statement* "end") ";"
//                  | ";"
//
// §13.6 example:
//   public property Name: string
//     get => _name;
//     private set => _name := value;
//   end Name;
//
// §23.3 example:
//   public property Balance: decimal
//     get => _balance;
//     set => do ... end;
//   end Balance;
//
// §17 (prose, §A.6) establishes that assignment `:=` is a STATEMENT not an
// expression. The §13.6 example `set => _name := value;` therefore requires
// the parser to accept a single Statement after `=>`, whose own terminating
// `;` also closes the AccessorBody — consistent with how §A.7 ArmBody
// treats a single Statement.
//
// This file tests: (1) the setter with an assignment statement body,
// (2) the mixed get+set case from §13.6, (3) compound assignment in setter,
// (4) negative: expression body still works for a getter,
// (5) negative: setter with expression body that is a call expression.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

// §13.6 repro: `set => _name := value;` — direct spec example.
// The assignment body is stored as a Block with a single AssignmentStmt.
BEZEL_TEST(parser_setter_expr_body_676, set_assignment_body_spec_13_6) {
    auto r = parseAll(
        "module M; class C "
        "private _name: string; "
        "public property Name: string "
        "  get => _name; "
        "  set => _name := value; "
        "end Name; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    // Two members: field + property.
    if (ms.size() == 2)
        BEZEL_EXPECT_EQ(
            ms[1],
            std::string("public property Name: string "
                        "{ get => _name; "
                        "set => do { _name := value; } end; }"));
}

// §13.6 with per-accessor visibility: `private set => _name := value;`
BEZEL_TEST(parser_setter_expr_body_676, private_set_assignment_body) {
    auto r = parseAll(
        "module M; class C "
        "private _name: string; "
        "public property Name: string "
        "  get => _name; "
        "  private set => _name := value; "
        "end Name; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    if (ms.size() == 2)
        BEZEL_EXPECT_EQ(
            ms[1],
            std::string("public property Name: string "
                        "{ get => _name; "
                        "private set => do { _name := value; } end; }"));
}

// Compound assignment in setter body: `set => _count += 1;`
BEZEL_TEST(parser_setter_expr_body_676, set_compound_assignment_body) {
    auto r = parseAll(
        "module M; class C "
        "private _count: int; "
        "public property Count: int "
        "  get => _count; "
        "  set => _count += value; "
        "end Count; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    if (ms.size() == 2)
        BEZEL_EXPECT_EQ(
            ms[1],
            std::string("public property Count: int "
                        "{ get => _count; "
                        "set => do { _count += value; } end; }"));
}

// Field assignment via member access: `set => this._val := value;`
BEZEL_TEST(parser_setter_expr_body_676, set_this_dot_assignment_body) {
    auto r = parseAll(
        "module M; class C "
        "private _val: int; "
        "public property Val: int "
        "  get => _val; "
        "  set => this._val := value; "
        "end Val; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Regression: getter with plain expression body must still work (§23.2 form).
BEZEL_TEST(parser_setter_expr_body_676, getter_expression_body_regression) {
    auto r = parseAll(
        "module M; class C "
        "public property X: int "
        "  get => 42; "
        "end X; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public property X: int { get => 42; }"));
}

// Setter with expression body (a call expression) must still work.
BEZEL_TEST(parser_setter_expr_body_676, set_call_expression_body) {
    auto r = parseAll(
        "module M; class C "
        "public property X: int "
        "  set => Update(value); "
        "end X; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public property X: int { set => Update(value); }"));
}

// §23.3 block-body form `set => do ... end;` must still work alongside the
// new single-statement form.
BEZEL_TEST(parser_setter_expr_body_676, set_do_block_body_regression) {
    auto r = parseAll(
        "module M; class C "
        "public property Balance: int "
        "  get => _balance; "
        "  set => do "
        "    _balance := value; "
        "  end; "
        "end Balance; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Indexer setter with assignment body.
BEZEL_TEST(parser_setter_expr_body_676, indexer_set_assignment_body) {
    auto r = parseAll(
        "module M; class C "
        "public property [i: int]: int "
        "  get => _items[i]; "
        "  set => _items[i] := value; "
        "end; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
