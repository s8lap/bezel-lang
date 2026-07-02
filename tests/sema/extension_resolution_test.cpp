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

// ---------------------------------------------------------------------------
// §26.3 (#936) — an extension block may not declare an operator overload.
// Before the fix the declaration was silently accepted (and ignored at call
// sites). It must now be rejected with a §26.3 diagnostic.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension, operator_overload_in_extension_rejected_936) {
    auto r = parseSemaAll(
        "module M; "
        "extension int "
        "  public static method operator +(a: int, b: int): int => a + b + 100; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("operator overload") != std::string::npos &&
            d.message.find("26.3") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §26.3 — a regular extension METHOD (not an operator) is still accepted.
BEZEL_TEST(sema_extension, plain_method_in_extension_still_accepted_936) {
    auto r = parseSemaAll(
        "module M; "
        "extension int public method twice(): int => this * 2; end; "
        "class C public static method Main(): int "
        "  var x: int := 21; return x.twice(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.3 (#1346, v0.160) — extension member visibility must be public or
// internal; `protected` and `private` are compile errors.
// ---------------------------------------------------------------------------

// §26.3 (#1346): `protected` extension method → error.
BEZEL_TEST(sema_extension, protected_method_in_extension_rejected_1346) {
    auto r = parseSemaAll(
        "module M; class Foo public _x: int := 0; end Foo; "
        "extension Foo protected method Bar(): int => this._x; end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("protected") != std::string::npos &&
            d.message.find("26.3") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §26.3 (#1346): `private` extension method → error.
BEZEL_TEST(sema_extension, private_method_in_extension_rejected_1346) {
    auto r = parseSemaAll(
        "module M; class Foo public _x: int := 0; end Foo; "
        "extension Foo private method Bar(): int => this._x; end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("private") != std::string::npos &&
            d.message.find("26.3") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §26.3 (#1346): `public` extension method → accepted (no error).
BEZEL_TEST(sema_extension, public_method_in_extension_accepted_1346) {
    auto r = parseSemaAll(
        "module M; class Foo public _x: int := 0; end Foo; "
        "extension Foo public method Bar(): int => this._x; end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §26.3 (#1346): `internal` (default) extension method → accepted (no error).
BEZEL_TEST(sema_extension, internal_method_in_extension_accepted_1346) {
    auto r = parseSemaAll(
        "module M; class Foo public _x: int := 0; end Foo; "
        "extension Foo internal method Bar(): int => this._x; end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.3 (#1346, v0.160) — extension properties must be expression-bodied.
// Auto-property and block-accessor forms are not admitted.
// ---------------------------------------------------------------------------

// §26.3 (#1346): auto-property in extension → error.
BEZEL_TEST(sema_extension, auto_property_in_extension_rejected_1346) {
    auto r = parseSemaAll(
        "module M; "
        "extension int public property X: int := 0; end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("26.3") != std::string::npos) found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §26.3 (#1346): expression-bodied property in extension → accepted (no error).
BEZEL_TEST(sema_extension, expr_property_in_extension_accepted_1346) {
    auto r = parseSemaAll(
        "module M; "
        "extension int public property Doubled: int => this * 2; end; "
        "class C public static method Main(): int "
        "  var x: int := 3; return x.Doubled; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §26.3 (#1346): regression — `extension Dictionary<K, V>` (two bare
// type-params) continues to work; no shadowing warning, no visibility error.
BEZEL_TEST(sema_extension, generic_extension_two_params_accepted_1346) {
    auto r = parseSemaAll(
        "module M; "
        "extension Dictionary<K, V> "
        "  public method IsEmpty(): bool => this.Count = 0; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §26.3 (#1346): regression — `extension IEnumerable<int>` (concrete arg)
// continues to work.
BEZEL_TEST(sema_extension, concrete_arg_extension_accepted_1346) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<int> "
        "  public method Sum(): int "
        "    var total: int := 0; "
        "    for var x in this do total := total + x; end; "
        "    return total; "
        "  end Sum; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.4 (#1346, v0.160) — shadowing warning when a bare type-param name in
// an extension block shadows a same-named type already in scope.
// ---------------------------------------------------------------------------

// §26.4 (#1346): bare param shadows an in-scope user type → warning.
BEZEL_TEST(sema_extension, type_param_shadows_in_scope_type_warns_1346) {
    auto r = parseSemaAll(
        "module M; "
        "record T(value: int); "   // T is now a type in scope
        "extension IEnumerable<T> "  // T here should warn: shadows record T
        "  public method First(): T "
        "    for var item in this do return item; end; "
        "    throw new InvalidOperationException(\"empty\"); "
        "  end First; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    // Must produce a WARNING (Severity::Warning) but not a compile error.
    bool hasWarning = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("shadows") != std::string::npos &&
            d.message.find("26.4") != std::string::npos)
            hasWarning = true;
    BEZEL_EXPECT_TRUE(hasWarning);
}

// §26.4 (#1346): bare param with no same-named type in scope → no warning.
BEZEL_TEST(sema_extension, type_param_no_shadow_no_warn_1346) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<T> "  // T is NOT in scope as a type
        "  public method First(): T "
        "    for var item in this do return item; end; "
        "    throw new InvalidOperationException(\"empty\"); "
        "  end First; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    // Must NOT produce a shadowing warning.
    bool hasWarning = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("shadows") != std::string::npos)
            hasWarning = true;
    BEZEL_EXPECT_FALSE(hasWarning);
}
