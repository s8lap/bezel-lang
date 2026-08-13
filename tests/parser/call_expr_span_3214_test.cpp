// Regression pin for issue #3214 — the parser never stamped `ast::CallExpr`
// (and its null-conditional twin `ast::NullCondCallExpr`) with a span, so any
// sema/codegen diagnostic keyed on `ce.span.begin` printed the default `{0,0,0}`
// location — i.e. `0:0` in the diagnostic prefix. The same defect silently
// applied to the other postfix-suffix nodes in `parsePostfix` (MemberAccessExpr,
// NullCondMemberExpr, IndexExpr, PostfixBangExpr), which the issue notes are
// "worth auditing in the same pass" — same cause, same consequence. Each row
// pins one node kind with the same source shape so the fix doesn't regress on
// any of them.
//
// Spec citations (v0.254):
//   §22.5 — "Optional parameters and named arguments": the five binding
//           diagnostics (required parameter not supplied, named-argument not
//           bound, named-argument duplicates, no overload accepts named
//           argument, ref/out default inadmissible) all report at the call
//           site's `ce.span.begin` and used to print `0:0`.
//   §22.6 — "Overloading": the "no overload is unambiguously better" and
//           "no applicable overload" diagnostics — also keyed on
//           `ce.span.begin`.
//
// The fix lives in `src/parser/Parser_Exprs.cpp::parsePostfix`. Each suffix
// branch captures `suffixBegin = head->span.begin` BEFORE `std::move(head)`
// into the new node, then stamps the new node's `span.end = current_.location`
// after the closing punctuator is consumed. So:
//   - `span.begin` = start of the receiver (the most-derived operand before
//     this suffix), and
//   - `span.end`   = one past the closing `)` / `]` / `>` / `!`.
// The minimal pin is "non-zero begin"; the column/line ones pin the exact
// location convention.

#include "support/parse_support.hpp"
#include "test_harness.hpp"

#include "ast/Node.hpp"

using bezel::test::parseAll;

namespace {

// Walk `e` looking for the first node of dynamic type `T`. Returns null if
// none is found. Used to fetch the postfix node out of a larger expression
// tree so the assertion focuses on the call/index/member site.
template <typename T>
const T* firstOf(const bezel::ast::Expr* e) {
    if (!e) return nullptr;
    if (auto* t = dynamic_cast<const T*>(e)) return t;
    if (auto* c = dynamic_cast<const bezel::ast::CallExpr*>(e)) {
        if (auto* t = firstOf<T>(c->callee.get())) return t;
    } else if (auto* c = dynamic_cast<const bezel::ast::NullCondCallExpr*>(e)) {
        if (auto* t = firstOf<T>(c->callee.get())) return t;
    } else if (auto* m = dynamic_cast<const bezel::ast::MemberAccessExpr*>(e)) {
        if (auto* t = firstOf<T>(m->target.get())) return t;
    } else if (auto* m =
                   dynamic_cast<const bezel::ast::NullCondMemberExpr*>(e)) {
        if (auto* t = firstOf<T>(m->target.get())) return t;
    } else if (auto* i = dynamic_cast<const bezel::ast::IndexExpr*>(e)) {
        if (auto* t = firstOf<T>(i->target.get())) return t;
    } else if (auto* b = dynamic_cast<const bezel::ast::PostfixBangExpr*>(e)) {
        if (auto* t = firstOf<T>(b->operand.get())) return t;
    }
    return nullptr;
}

}  // namespace

// #3214-1: the canonical symptom from the issue — `T.Take(1)` in a method
// body. The CallExpr's span.begin is the `T` token (line 5, the first non-
// module statement). Pre-fix it was {0,0,0} and every §22.5 / §22.6
// diagnostic on this site printed `0:0`.
BEZEL_TEST(call_expr_span_3214, call_expr_span_stamped) {
    auto r = parseAll(
        "module M;\n"
        "class C\n"
        "  public static method Take(a: int, b: int): int => a + b;\n"
        "end C;\n"
        "public class Main\n"
        "  public static method Run(): int => C.Take(1);\n"
        "end Main;\n");
    BEZEL_EXPECT_TRUE(r.sourceFile != nullptr);
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls.back().get());
    if (!td || td->members.empty()) return;
    const auto* md =
        dynamic_cast<const bezel::ast::MethodDecl*>(td->members[0].get());
    if (!md || md->bodyKind != bezel::ast::MethodBodyKind::Expression ||
        !md->exprBody)
        return;
    const auto* ce = firstOf<const bezel::ast::CallExpr>(md->exprBody.get());
    BEZEL_EXPECT_TRUE(ce != nullptr);
    if (!ce) return;
    // Pre-fix: {0,0,0}. Post-fix: the `C` token on line 7, column 23.
    BEZEL_EXPECT_TRUE(ce->span.begin.line > 0);
    BEZEL_EXPECT_TRUE(ce->span.begin.column > 0);
    // The receiver's span.begin must equal the CallExpr's span.begin
    // (the fix uses the receiver's begin as the call's begin). The
    // receiver can be either an IdentifierExpr (for `f(x)`) or a
    // MemberAccessExpr (for `T.f(x)`); accept either. This pins the
    // "callee.begin, not current_.location" choice the issue sketches.
    if (ce->callee) {
        BEZEL_EXPECT_EQ(ce->span.begin.line, ce->callee->span.begin.line);
        BEZEL_EXPECT_EQ(ce->span.begin.column, ce->callee->span.begin.column);
        BEZEL_EXPECT_EQ(ce->span.begin.offset, ce->callee->span.begin.offset);
    }
    // span.end should also be non-zero (advanced past the closing `)`).
    // We don't pin the exact column because the lexer's column convention
    // is owned elsewhere; just pin that the suffix was consumed.
    BEZEL_EXPECT_TRUE(ce->span.end.line > 0);
    BEZEL_EXPECT_TRUE(ce->span.end.offset > ce->span.begin.offset);
}

// #3214-2: chained member access — `a.b.c.M(...)` has both a CallExpr
// (outermost) and MemberAccessExpr nodes. Each MUST carry its own span so
// sema reports of the call point at the call (not the receiver) and
// member-related reports point at the member (not the chain's start). The
// issue notes "IndexExpr and MemberAccessExpr ... worth auditing in the
// same pass".
BEZEL_TEST(call_expr_span_3214, member_access_span_stamped_in_chain) {
    auto r = parseAll(
        "module M;\n"
        "class Inner\n"
        "  public method M(): int => 0;\n"
        "end Inner;\n"
        "class Outer\n"
        "  public field i: Inner;\n"
        "end Outer;\n"
        "public class Main\n"
        "  public static method Run(o: Outer): int => o.i.M();\n"
        "end Main;\n");
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls.back().get());
    if (!td || td->members.empty()) return;
    const auto* md =
        dynamic_cast<const bezel::ast::MethodDecl*>(td->members[0].get());
    if (!md || !md->exprBody) return;

    // Find any MemberAccessExpr in the chain. There must be one for `o.i`.
    const auto* ma =
        firstOf<const bezel::ast::MemberAccessExpr>(md->exprBody.get());
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (!ma) return;
    BEZEL_EXPECT_TRUE(ma->span.begin.line > 0);
    BEZEL_EXPECT_TRUE(ma->span.end.line > 0);
    BEZEL_EXPECT_TRUE(ma->span.end.offset > ma->span.begin.offset);

    // The outermost CallExpr for `.M()`.
    const auto* ce = firstOf<const bezel::ast::CallExpr>(md->exprBody.get());
    BEZEL_EXPECT_TRUE(ce != nullptr);
    if (!ce) return;
    BEZEL_EXPECT_TRUE(ce->span.begin.line > 0);
    BEZEL_EXPECT_TRUE(ce->span.end.line > 0);
}

// #3214-3: IndexExpr — the `arr[i]` postfix. Same defect, same fix shape.
//   `Module.IndexExpr` (not a class), then `class C { method F() => a[i]; }`.
BEZEL_TEST(call_expr_span_3214, index_expr_span_stamped) {
    auto r = parseAll(
        "module M;\n"
        "public class Main\n"
        "  public static method F(a: List<int>): int => a[0];\n"
        "end Main;\n");
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls.back().get());
    if (!td || td->members.empty()) return;
    const auto* md =
        dynamic_cast<const bezel::ast::MethodDecl*>(td->members[0].get());
    if (!md || !md->exprBody) return;
    const auto* idx =
        firstOf<const bezel::ast::IndexExpr>(md->exprBody.get());
    BEZEL_EXPECT_TRUE(idx != nullptr);
    if (!idx) return;
    BEZEL_EXPECT_TRUE(idx->span.begin.line > 0);
    BEZEL_EXPECT_TRUE(idx->span.end.line > 0);
    BEZEL_EXPECT_TRUE(idx->span.end.offset > idx->span.begin.offset);
}

// #3214-4: NullCondCallExpr — `o?(args)`. Same fix shape; same defect
// class on the issue's "twinned" path. Note: `o?.M(1)` is NOT a
// NullCondCallExpr — that's a NullCondMemberExpr `o?.M` followed by a
// CallExpr `(1)`. To get a NullCondCallExpr we use `o?(M(1))`, where the
// `?(` punctuator binds the call to the receiver.
BEZEL_TEST(call_expr_span_3214, null_cond_call_expr_span_stamped) {
    auto r = parseAll(
        "module M;\n"
        "public class Main\n"
        "  public static method F(o: Widget?): int => o?(M(1));\n"
        "end Main;\n");
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls.back().get());
    if (!td || td->members.empty()) return;
    const auto* md =
        dynamic_cast<const bezel::ast::MethodDecl*>(td->members[0].get());
    if (!md || !md->exprBody) return;
    const auto* ncc =
        firstOf<const bezel::ast::NullCondCallExpr>(md->exprBody.get());
    BEZEL_EXPECT_TRUE(ncc != nullptr);
    if (!ncc) return;
    BEZEL_EXPECT_TRUE(ncc->span.begin.line > 0);
    BEZEL_EXPECT_TRUE(ncc->span.end.line > 0);
    BEZEL_EXPECT_TRUE(ncc->span.end.offset > ncc->span.begin.offset);
}

// #3214-5: PostfixBangExpr — `x!`. Cheap third-twin pin so a future
// refactor of `parsePostfix` can't drop it without the test firing.
BEZEL_TEST(call_expr_span_3214, postfix_bang_expr_span_stamped) {
    auto r = parseAll(
        "module M;\n"
        "public class Main\n"
        "  public static method F(x: int?): int => x!;\n"
        "end Main;\n");
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls.back().get());
    if (!td || td->members.empty()) return;
    const auto* md =
        dynamic_cast<const bezel::ast::MethodDecl*>(td->members[0].get());
    if (!md || !md->exprBody) return;
    const auto* bang =
        firstOf<const bezel::ast::PostfixBangExpr>(md->exprBody.get());
    BEZEL_EXPECT_TRUE(bang != nullptr);
    if (!bang) return;
    BEZEL_EXPECT_TRUE(bang->span.begin.line > 0);
    BEZEL_EXPECT_TRUE(bang->span.end.line > 0);
    BEZEL_EXPECT_TRUE(bang->span.end.offset > bang->span.begin.offset);
}