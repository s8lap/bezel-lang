// Sema tests for extension-member resolution (§26.5, issue #433).
//
// §26.5: when a member access `x.M(...)` finds no instance member of M on x's
// type, the compiler resolves M against an `extension` block declared on x's
// type. Resolution rules verified here:
//   1. Instance members win. A real member of the same name is selected; the
//      same-named extension is NOT consulted (no shadow).
//   2. An extension method on a CLASS resolves cleanly (no "no member" error).
//   3. An extension method on a PRIMITIVE (`extension int`) resolves cleanly.
//   4. An extension does NOT leak to a different type — calling the extension's
//      method on an UNRELATED class is still an unknown-member error.
//   5. A genuinely-missing member (no instance member, no extension) is still
//      diagnosed (the #414 path is not weakened).

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// §26.5 step 2 — extension on a class resolves; no "no member" diagnostic.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension, method_on_class_resolves_clean) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "extension Foo public method Bar(): int => this._x + 1; end; "
        "class P public method Use(f: Foo): int => f.Bar(); end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §26.5 step 2 — extension on a primitive (`extension int`) resolves clean.
BEZEL_TEST(sema_extension, method_on_int_resolves_clean) {
    auto r = parseSemaAll(
        "module M; "
        "extension int public method Double(): int => this * 2; end; "
        "class P public method Use(): int "
        "  var x := 5; return x.Double(); "
        "end Use; end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.5 NO-SHADOW — a real instance member with the same name wins. The
// resolved callee symbol must be the INSTANCE method (parent == Foo), not the
// extension method (parent == null).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension, instance_member_wins_no_shadow) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public method Value(): int => 100; end Foo; "
        "extension Foo public method Value(): int => 1; end; "
        "class P public method Use(f: Foo): int => f.Value(); end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());

    // The call `f.Value()` must resolve to the instance method whose parent is
    // the Foo TypeSymbol — never to the parentless extension method.
    using namespace bezel::sema;
    // Locate the instance MethodSymbol on Foo named "Value".
    MethodSymbol* instanceValue = nullptr;
    for (const auto& s : r.ctx->symbols()) {
        auto* ms = dynamic_cast<MethodSymbol*>(s.get());
        if (!ms || ms->name != "Value") continue;
        if (ms->parent && ms->parent->name == "Foo") instanceValue = ms;
    }
    BEZEL_EXPECT_TRUE(instanceValue != nullptr);

    // Find the `f.Value()` member-access expr and check its resolved symbol.
    // We assert it is a MethodSymbol with a non-null parent (the instance one),
    // i.e. NOT the extension method (which has a null parent).
    // Walk the AST: P.Use's expression body is `f.Value()` → CallExpr whose
    // callee is the MemberAccessExpr we want.
    bool checked = false;
    for (const auto& top : r.sourceFile->topDecls) {
        auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(top.get());
        if (!td || td->name != "P") continue;
        for (const auto& m : td->members) {
            auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (!md || md->name != "Use" || !md->exprBody) continue;
            auto* call =
                dynamic_cast<const bezel::ast::CallExpr*>(md->exprBody.get());
            if (!call || !call->callee) continue;
            Symbol* callee = r.ctx->exprSymbol(*call->callee);
            auto* cms = dynamic_cast<MethodSymbol*>(callee);
            BEZEL_EXPECT_TRUE(cms != nullptr);
            // Instance member wins: the resolved method has a parent (Foo),
            // and is exactly the instance method, not the extension.
            BEZEL_EXPECT_TRUE(cms == instanceValue);
            BEZEL_EXPECT_TRUE(cms != nullptr && cms->parent != nullptr);
            checked = true;
        }
    }
    BEZEL_EXPECT_TRUE(checked);
}

// ---------------------------------------------------------------------------
// §26.5 — an extension does NOT leak to an unrelated type. Calling Foo's
// extension method `Bar` on a different class `Other` is still unknown-member.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension, extension_does_not_leak_to_other_type) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "class Other public _y: int := 0; end Other; "
        "extension Foo public method Bar(): int => this._x; end; "
        "class P public method Use(o: Other): int => o.Bar(); end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §26.5 — a genuinely-missing member (no instance member, no extension) is
// still diagnosed (#414 path not weakened by extension lookup).
BEZEL_TEST(sema_extension, missing_member_still_diagnosed) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "extension Foo public method Bar(): int => this._x; end; "
        "class P public method Use(f: Foo): int => f.Nope(); end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
