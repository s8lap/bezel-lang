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
// §26.3 (v0.183, #1581) — a `static` member in an extension block is a
// compile error. Before the fix this was silently accepted and even
// resolvable at call sites (`Widget.Zero()` ran) — an extension block adds
// receiver-directed members only; statics resolve against the extended
// type's own declarations (§22.12).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension, static_method_in_extension_rejected_1581) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public _n: int := 0; end Widget; "
        "extension Widget "
        "  public static method Zero(): Widget => new Widget(); "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("static") != std::string::npos &&
            d.message.find("26.3") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §26.3 (v0.183, #1581): a `static` extension PROPERTY is likewise rejected.
BEZEL_TEST(sema_extension, static_property_in_extension_rejected_1581) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public _n: int := 0; end Widget; "
        "extension Widget "
        "  public static property Zero: int => 0; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("static") != std::string::npos &&
            d.message.find("26.3") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §26.3 (v0.183, #1581): a plain (non-static) instance extension method on a
// user-defined class continues to compile cleanly — the fix targets only
// the `static` modifier, not extension methods in general.
BEZEL_TEST(sema_extension, instance_method_in_extension_still_accepted_1581) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public _n: int := 0; end Widget; "
        "extension Widget "
        "  public method Describe(): int => this._n + 1; "
        "end; "
        "class C public static method Main(w: Widget): int "
        "  return w.Describe(); "
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

// ---------------------------------------------------------------------------
// §26.5 step 1 (v0.183, issue #1580 (a)) — a NAME-BLOCKED call (an instance
// member of the same name exists, so extension lookup is never consulted,
// §26.5 step 1) that then fails overload resolution must NOTE that a
// same-named extension exists but was not considered — without the note the
// caller cannot tell their extension was shadowed rather than simply absent.
// ---------------------------------------------------------------------------

// (a) — no-applicable-overload path (§22.6, via resolveMethodOverload):
// `Foo` declares two instance overloads of `Bar` (string, bool); calling with
// an `int` argument is applicable to neither (§5.1 has no int→string or
// int→bool implicit conversion — ExprTC::primitiveWidens). A same-named
// extension `Bar(x: int)` on `Foo` exists but is name-blocked (§26.5 step 1)
// and must never be considered — the diagnostic should say so.
BEZEL_TEST(sema_extension,
           name_blocked_no_applicable_overload_notes_extension_1580a) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo "
        "  public method Bar(x: string): int => 1; "
        "  public method Bar(x: bool): int => 2; "
        "end Foo; "
        "extension Foo public method Bar(x: int): int => 99; end; "
        "class P public method Use(f: Foo): int => f.Bar(5); end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool foundOverloadError = false;
    bool foundNote = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("no overload of") != std::string::npos &&
            d.message.find("22.6") != std::string::npos)
            foundOverloadError = true;
        if (d.message.find("extension") != std::string::npos &&
            d.message.find("26.5") != std::string::npos &&
            d.message.find("Bar") != std::string::npos)
            foundNote = true;
    }
    BEZEL_EXPECT_TRUE(foundOverloadError);
    BEZEL_EXPECT_TRUE(foundNote);
}

// (a) — regression: the SAME no-applicable-overload call, but with NO
// same-named extension declared anywhere, must NOT fabricate a note (empty
// extensionNameBlockedNote — no spurious "26.5"/"extension" text).
BEZEL_TEST(sema_extension,
           no_applicable_overload_without_extension_no_note_1580a) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo "
        "  public method Bar(x: string): int => 1; "
        "  public method Bar(x: bool): int => 2; "
        "end Foo; "
        "class P public method Use(f: Foo): int => f.Bar(5); end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool foundNote = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("no overload of") != std::string::npos &&
            d.message.find("26.5") != std::string::npos)
            foundNote = true;
    BEZEL_EXPECT_FALSE(foundNote);
}

// (a) — inaccessible-member path (§13.1, via checkMemberVisibility): `Secret`
// is `private` on `Foo`, so calling it from an unrelated class `P` is a
// §13.1 accessibility error. A same-named PUBLIC extension `Secret()` on
// `Foo` exists but is name-blocked (§26.5 step 1: instance members block by
// NAME regardless of accessibility) — the diagnostic should note it.
BEZEL_TEST(sema_extension,
           name_blocked_inaccessible_member_notes_extension_1580a) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo "
        "  private method Secret(): int => 42; "
        "end Foo; "
        "extension Foo public method Secret(): int => 1; end; "
        "class P public method Use(f: Foo): int => f.Secret(); end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool foundPrivateError = false;
    bool foundNote = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("private") != std::string::npos &&
            d.message.find("13.1") != std::string::npos)
            foundPrivateError = true;
        if (d.message.find("extension") != std::string::npos &&
            d.message.find("26.5") != std::string::npos &&
            d.message.find("Secret") != std::string::npos)
            foundNote = true;
    }
    BEZEL_EXPECT_TRUE(foundPrivateError);
    BEZEL_EXPECT_TRUE(foundNote);
}

// ---------------------------------------------------------------------------
// §26.4 (v0.183, issue #1580 (b)) — a NULLABLE-MARKED identifier in an
// extension's target-type argument position (`extension Array<T?>`) binds no
// type parameter and is ALWAYS a compile error naming this rule. Before the
// fix this fell through to the generic "unresolved type name `T`" diagnostic
// (misleading — T is not a typo, it occupies exactly a type-parameter-head
// position) with no §26.4 citation at all.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_extension, nullable_marked_head_cites_26_4_1580b) {
    auto r = parseSemaAll(
        "module M; "
        "extension Array<T?> "
        "  public method Describe(): int => 0; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool foundOldMisleadingError = false;
    bool foundSpecCitedError = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("unresolved type name") != std::string::npos)
            foundOldMisleadingError = true;
        if (d.message.find("26.4") != std::string::npos &&
            d.message.find("nullable") != std::string::npos)
            foundSpecCitedError = true;
    }
    // The old bug: falls through to the generic "unresolved type name"
    // diagnostic. Must be gone.
    BEZEL_EXPECT_FALSE(foundOldMisleadingError);
    // The fix: a diagnostic naming the actual §26.4 rule.
    BEZEL_EXPECT_TRUE(foundSpecCitedError);
}

// (b) — regression: a bare, non-nullable `extension Array<T>` (the legal
// form) must still resolve cleanly — the new nullable-only branch must not
// disturb the existing non-nullable binding path.
BEZEL_TEST(sema_extension, bare_non_nullable_array_head_still_accepted_1580b) {
    auto r = parseSemaAll(
        "module M; "
        "extension Array<T> "
        "  public method IsSingle(): bool => this.Count = 1; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (b) — regression: a genuinely concrete nullable argument, `Array<int?>`
// used as a CALLER's variable type (not an extension head), is unaffected —
// the new §26.4 rule only fires on an extension's OWN target-type argument
// position, never on ordinary type resolution elsewhere.
BEZEL_TEST(sema_extension, concrete_nullable_type_elsewhere_unaffected_1580b) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var xs: Array<int?>?; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.5 step 2 (v0.183, issue #1580 (c)) — visibility filtering. §26.3: "From
// the same module, internal members are also visible" and §13.1: `internal`
// = same module. Every extension the compiler currently locates for a call
// site comes from `ctx_.module()->children` — i.e. the CALLING module itself
// (see ExprTC::lookupExtensionMember) — so an `internal` extension member is
// always same-module-visible at any call site that can find it at all; step
// 2's visibility filter is correctly a no-op in the only case reachable
// today. (Cross-module extension import — the case where filtering would
// actually need to REMOVE a candidate — is separately unimplemented; see
// ModuleMetadataBuilder.cpp's "Type aliases, extensions, and extern blocks
// are likewise out of this increment's scope" and SymbolCollection.cpp /
// ExprTypecheck.cpp, which only ever populate/search ExtensionSymbols on the
// current module. That is a larger, already-flagged prerequisite, not a
// diagnostics defect, and is out of scope here.) This test locks in that an
// `internal` extension member is reachable — same-module visibility must
// never be wrongly rejected.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension,
           internal_extension_member_visible_same_module_1580c) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "extension Foo internal method Bar(): int => this._x; end; "
        "class P public method Use(f: Foo): int => f.Bar(); end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.5 rule 4 / §26.5.1 (issue #1307) — the ambiguity diagnostic.
//
// §26.5.1: "If neither candidate's substituted head is a proper subtype of
// the other's, the candidates are equally specific and rule 4 of §26.5
// produces an ambiguity error. In particular, candidates with IDENTICAL
// substituted heads never auto-rank: neither a more-concrete-looking
// declared head (`extension IEnumerable<int>` and `extension IEnumerable<T>`
// are indistinguishable after substitution for an `Array<int>` receiver) nor
// a stronger `where` clause on the same head makes one candidate more
// specific than the other."
//
// §26.5 rule 4: "If two equally-specific extensions match, the call is a
// compile error. Because extension blocks are nameless, the diagnostic must
// identify each candidate by what it has: its defining module, its declared
// head including any `where` clause, and its declaration site."
//
// The runtime (would-select-100-or-200) half of this pair — the STRICT-
// subtype, non-tied case — is covered by the codegen tests in
// tests/codegen/extension_specificity_1307_test.cpp; these sema-level tests
// cover the case those RUNTIME tests cannot (a rejected program has no
// runtime), and assert against the actual diagnostic TEXT rather than
// accepting any compile error, per the rule-4 candidate-identifying
// requirement above.
// ---------------------------------------------------------------------------

// The spec's own worked example (§26.5.1, second code block): `extension
// IEnumerable<T>` and `extension IEnumerable<int>` both substitute to the
// SAME head `IEnumerable<int>` for an `Array<int>` receiver — identical
// substituted heads, so neither is a proper subtype of the other.
BEZEL_TEST(sema_extension,
           rule4_identical_substituted_heads_ambiguous_spec_26_5_1_1307) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<T> public method Pick(): int => 100; end; "
        "extension IEnumerable<int> public method Pick(): int => 200; end; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.Pick(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("ambiguous extension member") != std::string::npos &&
            d.message.find("§26.5 rule 4") != std::string::npos &&
            d.message.find("extension IEnumerable<T>") != std::string::npos &&
            d.message.find("extension IEnumerable<int>") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §26.5.1: "nor a stronger `where` clause on the same head makes one
// candidate more specific than the other" — a `where`-constrained block does
// NOT auto-beat the unconstrained same-head block; both substitute to
// `IEnumerable<int>` for an `Array<int>` receiver (a `where` clause is "an
// APPLICABILITY filter, never a ranking signal"), so this is also a tie.
BEZEL_TEST(sema_extension,
           rule4_constrained_vs_unconstrained_same_head_ambiguous_spec_26_5_1_1307) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<T> public method Pick(): int => 100; end; "
        "extension IEnumerable<T> where T: IComparable<T> "
        "  public method Pick(): int => 200; "
        "end; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.Pick(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("ambiguous extension member") != std::string::npos &&
            d.message.find("§26.5 rule 4") != std::string::npos &&
            d.message.find("where T: IComparable<T>") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// A tie need not share a head at all: two UNRELATED interfaces (neither a
// subtype of the other) both implemented by the receiver's class — §26.5.1's
// "neither candidate's substituted head is a proper subtype of the other's"
// clause covers incomparable heads, not just identical ones.
BEZEL_TEST(sema_extension,
           rule4_unrelated_interfaces_ambiguous_spec_26_5_1307) {
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo end IFoo; "
        "interface IBar end IBar; "
        "extension IFoo public method Pick(): int => 10; end; "
        "extension IBar public method Pick(): int => 20; end; "
        "class Widget: IFoo, IBar end Widget; "
        "class Runner public static method Main(): int "
        "  var w := new Widget(); "
        "  return w.Pick(); "
        "end Main; end Runner;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("ambiguous extension member") != std::string::npos &&
            d.message.find("§26.5 rule 4") != std::string::npos &&
            d.message.find("extension IFoo") != std::string::npos &&
            d.message.find("extension IBar") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// The diagnostic must name each tied candidate's defining module (rule 4:
// "its defining module, its declared head including any `where` clause, and
// its declaration site") — both candidates here are declared in the same
// (single, local) module `M`, so the module name `M` must appear once per
// candidate line.
BEZEL_TEST(sema_extension,
           rule4_diagnostic_names_defining_module_1307) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<T> public method Pick(): int => 100; end; "
        "extension IEnumerable<int> public method Pick(): int => 200; end; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.Pick(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("ambiguous extension member") != std::string::npos &&
            d.message.find("module `M`") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// §26.4 (v0.303, issue #4448) — a bare identifier in an extension head's
// FUNCTION-TYPE slot (`extension Array<(T) -> T>`) binds NO type parameter
// and is a compile error naming §26.4 — not the generic "unresolved type
// name `T`" fallback the compiler used to emit (and emitted twice, once per
// occurrence of `T`). The fix detects the function-type slot during the
// §26.4 type-parameter-binding walk
// (`Resolver::bindExtensionTargetTypeParams`), reports the §26.4 diagnostic
// exactly once at the offending identifier, and suppresses the redundant
// "unresolved type name" cascade. The diagnostic message names the rule
// (§26.4), the slot class ("function slot"), and the reason ("a function
// type's parameter and return slots are not argument positions").
//
// The spec's own examples are `extension Array<(T) -> T>` and the nested
// twin `extension IEnumerable<Array<(T) -> T>>` — both must hit the
// new diagnostic, and BOTH must produce it exactly once.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_extension,
           function_slot_bare_identifier_cites_26_4_4448) {
    auto r = parseSemaAll(
        "module M; "
        "extension Array<(T) -> T> "
        "  public method NoOp(): int => 0; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    int section264Hits = 0;
    bool hasFunctionSlotHit = false;
    bool hasUnresolvedTypeName = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("\xC2\xA7""26.4") != std::string::npos) {
            ++section264Hits;
            if (d.message.find("function slot") != std::string::npos)
                hasFunctionSlotHit = true;
        }
        if (d.message.find("unresolved type name") != std::string::npos)
            hasUnresolvedTypeName = true;
    }
    // The §26.4 diagnostic must fire — at least once, with the spec
    // section citation and the slot-class word. The "function slot"
    // wording names the structural shape the rule covers, distinguishing
    // it from the existing nullable-marked and bare-identifier cases.
    BEZEL_EXPECT_TRUE(hasFunctionSlotHit);
    // The fix requires EXACTLY one diagnostic per extension head, not
    // one per bare identifier. The pre-fix output was two
    // `unresolved type name `T`` errors (one per `T` occurrence).
    BEZEL_EXPECT_EQ(section264Hits, 1);
    // The pre-fix "unresolved type name `T`" cascade must be gone — the
    // §26.4 rule is the diagnostic of record.
    BEZEL_EXPECT_FALSE(hasUnresolvedTypeName);
}

BEZEL_TEST(sema_extension,
           function_slot_nested_in_generic_arg_cites_26_4_4448) {
    // The spec's nesting-clause composed example:
    // `extension IEnumerable<Array<(T) -> T>>` binds nothing either.
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<Array<(T) -> T>> "
        "  public method NoOp(): int => 0; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    int section264Hits = 0;
    bool hasFunctionSlotHit = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("\xC2\xA7""26.4") != std::string::npos) {
            ++section264Hits;
            if (d.message.find("function slot") != std::string::npos)
                hasFunctionSlotHit = true;
        }
    }
    BEZEL_EXPECT_TRUE(hasFunctionSlotHit);
    // Nested case: still EXACTLY one diagnostic per extension head.
    BEZEL_EXPECT_EQ(section264Hits, 1);
}

// §26.4 accept-control (per the issue's "Regression tests" section):
// `extension IEnumerable<Array<T>>` is the spec's own worked example of
// binding T through a nested generic argument — the new function-slot
// diagnostic must NOT fire on a bare identifier inside a real argument
// position even when nested under another generic instantiation.
BEZEL_TEST(sema_extension,
           nested_bare_identifier_in_arg_position_still_binds_4448) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<Array<T>> "
        "  public method FirstCount(): int => this.Count; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    // Note: the HEAD is what this row is about — `T` sits in
    // `Array<...>`'s argument position, so §26.4's binding walk reaches it
    // and the function-slot diagnostic must not fire. The BODY is
    // incidental: `this.Count` names no member, because stdlib §4.12
    // declares `IEnumerable<out T>` with exactly one member
    // (`GetEnumerator`) and the count is stdlib §5's
    // `method Count(): int` — a METHOD, spelled with parentheses (#6389).
    // So the program is correctly REJECTED, and `!hasErrors()` would make
    // the body shape load-bearing on a row whose stated target is the head.
    // We assert the function-slot diagnostic specifically — as our
    // Dictionary sibling below already does — and additionally pin the
    // diagnostic that DOES fire.
    //
    // WHY conjunct (2) is here — the anti-vacuity guard, NOT a head-binding
    // claim. `BEZEL_EXPECT_FALSE(sawFunctionSlot)` on its own is satisfied by
    // any program that never reached §26.4's walk: dropping the `>` in this
    // head yields parse diagnostics and no function-slot one, and the absence
    // conjunct still passes. Pinning that one specific diagnostic DID fire,
    // and that it was the ONLY one, is what makes the absence above mean
    // "§26.4 declined to call this a function slot" rather than "nothing
    // got far enough to say".
    //
    // It is NOT evidence that the head bound T, and an earlier revision of
    // this comment claimed it was. Conjunct (2) is INSENSITIVE to how the
    // argument positions were spelled: `extension IEnumerable<Array<NoSuchType>>`
    // yields the same single member diagnostic as this head, byte for byte
    // apart from the filename.
    //
    // That equality is NOT the compiler failing to bind. §26.4 (v0.338,
    // *Type-parameter binding*) binds both spellings: "In the extended type's
    // argument positions, each bare single-segment non-nullable identifier
    // introduces a fresh block-level type parameter — always, shadowing any
    // same-named type in scope", and it declines the alternative — "resolving
    // the identifier against scope and only binding when unresolved" —
    // "is declined: it would let an unrelated type declaration elsewhere
    // silently re-mean an extension block" (the ruling is recorded again at
    // changelog v0.160, #1346). So `NoSuchType` in that head is a bound
    // parameter spelled with a type-shaped word, not a name that failed to
    // resolve, and no diagnostic is owed for it. The compiler says so itself
    // where it can: with a same-named type in scope, `extension Array<NoSuchType>`
    // gives "warning: type parameter `NoSuchType` in this extension block
    // shadows the type `NoSuchType` that is in scope (§26.4); the extension
    // type parameter always takes precedence".
    //
    // It is a usable parameter, not a placeholder: MEASURED, an
    // `extension Array<NoSuchType>` block using it as a parameter type, a
    // return type and a local type compiles with no diagnostic and RUNS,
    // inferring `NoSuchType` as `int` from the receiver and returning the
    // passed value — the same program spelled `T` behaving identically. The
    // controls that DO fire show the compiler is not lax about names in
    // signatures: the same identifier qualified in an argument position
    // (`Array<NoSuchType.Nope>`) or used as the head itself
    // (`extension NoSuchTypeX<T>`) is diagnosed `unresolved type name`.
    //
    // What is still open is a usability question, not a gap here: §26.4's
    // one affordance is the shadowing warning, and it needs a same-named type
    // in scope, so an invented name is silent. Tracked at #6522
    // (`spec-question`, OPEN). #6518 asked for this to be diagnosed and was
    // closed `not planned` — spec-mandated, per the same §26.4 clause.
    // Do not re-derive a head-binding guarantee from conjunct (2).
    //
    // Both conjuncts are non-vacuous:
    //   (1) fails on `extension IEnumerable<Array<(T) -> T>>` — the same
    //       body under a head that DOES put T in a function slot, which
    //       emits "…function slot… (§26.4)".
    //   (2) fails if #6389's rejection is withdrawn, or if any second
    //       diagnostic appears.
    int diagCount = 0;
    bool sawFunctionSlot = false;
    bool sawCountMember = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        ++diagCount;
        if (d.message.find("function slot") != std::string::npos)
            sawFunctionSlot = true;
        if (d.message.find("no member `Count`") != std::string::npos &&
            d.message.find("IEnumerable<T>") != std::string::npos)
            sawCountMember = true;
    }
    // (1) T is in an argument position: the §26.4 function-slot diagnostic
    // must NOT fire. This is the row's documented purpose.
    BEZEL_EXPECT_FALSE(sawFunctionSlot);
    // (2) and the single diagnostic that does fire is #6389's stdlib §5
    // operator member diagnostic. This is the ANTI-VACUITY guard, not a
    // claim that the head bound T — see the measurement in the comment
    // above.
    BEZEL_EXPECT_TRUE(sawCountMember);
    BEZEL_EXPECT_EQ(diagCount, 1);
}

// §26.4 accept-control (per the issue's "Regression tests" section):
// `extension Dictionary<K, Array<V>>` is the spec's other worked example —
// two bare identifiers in argument positions at DISTINCT NESTING DEPTHS
// (`K` in `Dictionary`'s own argument position, `V` one level deeper inside
// `Array<…>`). The function-slot diagnostic must not fire here either:
// neither identifier sits in a function type's parameter or return slot, so
// §26.4 binds both.
//
// The program as originally written (issue #6517) was a PARSE ERROR — its
// single `end;` closed the METHOD, not the extension, so the extension block
// was never closed and `class C` parsed as a nested declaration. Three
// PARSER diagnostics came out, the §26.4 walk never ran, and the row's
// assertion — that no diagnostic contains "function slot" — was satisfied by
// a program that never reached sema. It could not fail: delete §26.4's
// function-slot diagnostic outright and it still passed. The `end;` closing
// the method (and the `end;` closing the extension) are load-bearing here,
// and the comment says why.
//
// Four conjuncts, each falsifiable — see the negative-control row below,
// which drives this same body to this same diagnostic on a head one slot-move
// away:
//
//   (1) the §26.4 function-slot diagnostic must NOT fire — the row's
//       documented purpose, and the conjunct the negative control breaks.
//   (2) NEITHER identifier may fail to bind. `V` is load-bearing in the body:
//       the parameter is typed `Array<V>`, so a head that bound `K` but not
//       `V` resolves the body's `V` against module scope and emits
//       "unresolved type name `V`". This is the conjunct separating "both
//       bound" from "both silently unbound" — the head-binding question this
//       row is actually about.
//   (3) the diagnostic that DOES fire is pinned POSITIVELY and by name. The
//       member read is deliberately a PROPERTY (`this.FirstKey`, no
//       parentheses): stdlib §4.8 declares `Count`/`Keys`/`Values`/`Add`/
//       `ContainsKey`/`Remove`/`TryGetValue`/`GetOrNull`/`GetOrAdd`/`Clear`
//       on `Dictionary`, and `FirstKey` is `SortedDictionary`'s (§4.17), so
//       the property read is REJECTED at sema with a diagnostic naming the
//       receiver type. (The CALL spelling `this.FirstKey()` reaches no sema
//       member check at all — it falls through to codegen's #2457 "produced
//       no value" backstop, which this in-process parse+sema helper does not
//       run — so the call spelling would leave conjunct (3) with nothing to
//       pin.) A program emitting NO diagnostic at all satisfies (1) and (2)
//       vacuously; (3) forbids that, and it is also what would have caught
//       the parse-error program.
//   (4) exactly one diagnostic — so a second one appearing (a head-binding
//       regression, say) is caught rather than absorbed.
//
// The body shape is incidental (as the original comment said), but it is now
// PINNED rather than tolerated: it is the evidence that sema ran.
BEZEL_TEST(sema_extension,
           two_bare_identifiers_in_nested_args_still_both_bind_4448) {
    auto r = parseSemaAll(
        "module M; "
        "extension Dictionary<K, Array<V>> "
        "  public method TakeFrom(values: Array<V>): K "
        "    var key := this.FirstKey; "
        "    return key; "
        "  end; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    int diagCount = 0;
    bool sawFunctionSlot = false;
    bool sawUnresolvedTypeName = false;
    bool sawFirstKeyMember = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        ++diagCount;
        if (d.message.find("function slot") != std::string::npos)
            sawFunctionSlot = true;
        if (d.message.find("unresolved type name") != std::string::npos)
            sawUnresolvedTypeName = true;
        // The stdlib §4.8 "not part of this collection's documented surface"
        // diagnostic, pinned by BOTH halves: the member name AND the receiver
        // type it was looked up on. Naming the receiver type is what shows the
        // head resolved `Dictionary` with `K`/`V` bound, rather than the
        // lookup happening on some other type.
        if (d.message.find("no member `FirstKey`") != std::string::npos &&
            d.message.find("`Dictionary`") != std::string::npos)
            sawFirstKeyMember = true;
    }
    // (1) Neither identifier is in a function slot, so §26.4 binds both and
    // the function-slot diagnostic must not fire. This conjunct is the row's
    // documented purpose.
    BEZEL_EXPECT_FALSE(sawFunctionSlot);
    // (2) Both identifiers actually bound — an unbound `V` surfaces in the
    // body as "unresolved type name `V`".
    BEZEL_EXPECT_FALSE(sawUnresolvedTypeName);
    // (3) The one diagnostic that does fire is the stdlib §4.8 member
    // diagnostic on `Dictionary`, which is what proves sema ran at all.
    BEZEL_EXPECT_TRUE(sawFirstKeyMember);
    // (4) and it is the only one.
    BEZEL_EXPECT_EQ(diagCount, 1);
}

// §26.4 NEGATIVE CONTROL for the row above (#6517). The accept-control's
// conjunct (1) is an ABSENCE, so on its own it cannot show that the
// diagnostic it forbids is reachable at all. This row puts the §26.4
// function-slot diagnostic back into the very head the accept-control
// blesses — one slot-move away, same body, same helper — and asserts the
// diagnostic DOES fire. If §26.4's function-slot diagnostic were withdrawn, or
// its argument-position walk stopped reaching these positions, THIS row goes
// red while the accept-control stays green: the pair is what makes the
// accept-control's absence a claim about a live diagnostic rather than a
// tautology.
//
// Both spellings are covered because the accept-control's subject is two
// identifiers at two distinct depths: `K` at `Dictionary`'s OWN argument
// position, `V` nested inside `Array<…>`. Moving either into a function slot
// must be rejected — §26.4's nesting clause composes ONLY through generic
// arguments, so neither depth is special.
BEZEL_TEST(sema_extension,
           two_bare_identifiers_nested_args_control_function_slot_fires_4448) {
    // Control A: `V` moved into a function slot inside the nested `Array<…>`.
    {
        auto r = parseSemaAll(
            "module M; "
            "extension Dictionary<K, Array<(V) -> V>> "
            "  public method TakeFrom(values: Array<V>): K "
            "    var key := this.FirstKey; "
            "    return key; "
            "  end; "
            "end; "
            "class C public static method Main(): int return 0; end Main; end C;");
        bool sawFunctionSlot = false;
        int section264Hits = 0;
        for (const auto& d : r.diagnostics.diagnostics()) {
            if (d.message.find("\xC2\xA7""26.4") != std::string::npos)
                ++section264Hits;
            if (d.message.find("function slot") != std::string::npos)
                sawFunctionSlot = true;
        }
        BEZEL_EXPECT_TRUE(sawFunctionSlot);
        BEZEL_EXPECT_EQ(section264Hits, 1);
    }
    // Control B: `K` moved into a function slot at `Dictionary`'s OWN argument
    // position. (This spelling draws a second, independent diagnostic — the
    // §3.6/§8.3 hashable-key-type rejection, since a function type is not
    // hashable — which is why this control asserts on the §26.4 function-slot
    // conjunct and its count, not on a total diagnostic count.)
    {
        auto r = parseSemaAll(
            "module M; "
            "extension Dictionary<(K) -> K, Array<V>> "
            "  public method TakeFrom(values: Array<V>): K "
            "    var key := this.FirstKey; "
            "    return key; "
            "  end; "
            "end; "
            "class C public static method Main(): int return 0; end Main; end C;");
        bool sawFunctionSlot = false;
        int section264Hits = 0;
        for (const auto& d : r.diagnostics.diagnostics()) {
            if (d.message.find("\xC2\xA7""26.4") != std::string::npos)
                ++section264Hits;
            if (d.message.find("function slot") != std::string::npos)
                sawFunctionSlot = true;
        }
        BEZEL_EXPECT_TRUE(sawFunctionSlot);
        BEZEL_EXPECT_EQ(section264Hits, 1);
    }
}
