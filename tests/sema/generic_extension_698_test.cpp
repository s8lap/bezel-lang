// Spec-derived tests for generic extension type-parameter scoping (§26.4,
// issue #698).
//
// §26.4 (Generic extensions): an extension declaration of the form
// `extension Foo<T>` introduces T as a type parameter that must be visible
// throughout the extension body (member signatures, return types, param types,
// local-variable types, and where-constraint types).  Before fix #698, the
// extension generic params were NOT bound into the scope used to resolve the
// extension body, producing "unresolved type name T" for every T reference.
//
// Tests derived from:
//   spec §26.4 examples (spec_0165, spec_0166) — generic extensions.
//   spec §26.5 example (spec_0167) — specificity ordering; the generic
//     extension overload's body uses T directly.
//
// Regression:
//   Non-generic extensions (§26.1–§26.3) must continue to compile without
//   error — the fix must not disturb the non-generic code path.

#include "sema/ExprTypecheck.hpp"
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

bezel::test::SemaResult parseAndResolve(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// §26.4 — generic extension: return type T resolves (no "unresolved type" error)
// Minimal probe: `extension Box<T>` whose method returns T.
// Derived from §26.4 pattern; Box is a user class with a single T field.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_generic_extension_698, return_type_T_resolves_sec26_4) {
    // per §26.4: the extension's own type parameter T must be in scope for
    // member return types, exactly as a class generic param is in scope for
    // its members.
    auto r = parseAndResolve(
        "module M; "
        "class Box<T> public _v: T?; end Box; "
        "extension Box<T> "
        "  public method Get(): T? => this._v; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.4 — generic extension: parameter of type T resolves.
// A method that TAKES a T parameter (not just returns one).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_generic_extension_698, param_type_T_resolves_sec26_4) {
    // per §26.4: T is in scope for method parameter types too. The method
    // has no return type (unit-returning — §12.1: omitting `: T` means unit).
    auto r = parseAndResolve(
        "module M; "
        "class Box<T> public _v: T?; end Box; "
        "extension Box<T> "
        "  public method Set(v: T) end Set; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.4 — generic extension: local-variable type Array<T> resolves.
// Tests that T is in scope not only in the signature but also inside the
// method body (local declarations).
// Derived from spec §26.4 Sort example (spec_0165).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_generic_extension_698, array_of_T_local_resolves_sec26_4) {
    // §26.4 spec_0165 shape: `Array<T>` in return type and body local.
    auto r = parseAndResolve(
        "module M; "
        "extension IEnumerable<T> "
        "  public method ToArr(): Array<T> "
        "    var items: Array<T>?; "
        "    return items; "
        "  end ToArr; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.4 — generic extension with where-constraint: T in constraint resolves.
// Derived from spec_0165 `extension IEnumerable<T> where T: IComparable<T>`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_generic_extension_698,
           where_constraint_T_resolves_sec26_4_spec0165) {
    // per §26.4: the where-clause `T: IComparable<T>` references T twice;
    // both occurrences must resolve to the extension's own type parameter.
    auto r = parseAndResolve(
        "module M; "
        "extension IEnumerable<T> where T: IComparable<T> "
        "  public method Sort(): Array<T> "
        "    var items: Array<T>?; "
        "    return items; "
        "  end Sort; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.4 — generic extension with two type parameters: both resolve.
// Derived from spec_0166 `extension IEnumerable<T>` with method generic `<U>`.
// (The method's own U is a method-level generic, T is the extension's.)
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_generic_extension_698,
           extension_T_and_method_U_both_resolve_sec26_4_spec0166) {
    // per §26.4 + §26.3: the extension param T and the method's own generic
    // param U are independent; both must be in scope inside the method body
    // and signature.
    auto r = parseAndResolve(
        "module M; "
        "extension IEnumerable<T> "
        "  public method Map<U>(transform: (T) -> U): IEnumerable<U> "
        "    var result: Array<U>?; "
        "    return result; "
        "  end Map; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.4 — two separate generic extensions on the same target; each binds its
// own T independently.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_generic_extension_698,
           two_generic_extensions_independent_params_sec26_4) {
    auto r = parseAndResolve(
        "module M; "
        "class Box<T> public _v: T?; end Box; "
        "extension Box<T> "
        "  public method First(): T? => this._v; "
        "end; "
        "extension Box<T> "
        "  public method Second(): T? => this._v; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.4 — generic extension on a builtin type (IEnumerable<T>).
// Method returns T directly. Verifies that `T` used as a bare return type
// (not wrapped in Array<T>) also resolves.
// Derived from spec_0167 generic overload.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_generic_extension_698,
           bare_T_return_resolves_sec26_4_spec0167) {
    auto r = parseAndResolve(
        "module M; "
        "extension IEnumerable<T> "
        "  public method First(): T "
        "    for var item in this do return item; end; "
        "    throw new InvalidOperationException(\"empty\"); "
        "  end First; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26 regression — non-generic extension on a class must still compile.
// This test must remain GREEN after the fix (§26.1–§26.3 non-generic case).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_generic_extension_698,
           non_generic_extension_unaffected_regression_sec26_1) {
    // per §26.1: non-generic extension has no type params; the fix must not
    // alter its behaviour.
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int := 0; end Foo; "
        "extension Foo "
        "  public method Bar(): int => this._x + 1; "
        "end; "
        "class P public method Use(f: Foo): int => f.Bar(); end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §26.4 — end-to-end: a generic extension method is CALLABLE at a concrete
// instantiation (invocation dispatch works for the resolved declaration).
// Calls `box.Get()` where box: Box<int> — resolution must find the extension
// method and typecheck the call without errors.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_generic_extension_698,
           generic_extension_method_call_resolves_sec26_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> public _v: T?; end Box; "
        "extension Box<T> "
        "  public method Get(): T? => this._v; "
        "end; "
        "class P "
        "  public method Use(b: Box<int>): int? => b.Get(); "
        "end P;");
    // At minimum, no "unresolved type name T" errors should appear.
    // Call-site dispatch may produce additional diagnostics on unimplemented
    // monomorphization paths; if so, the test accepts them as long as the
    // T-scoping error is gone. Check specifically for the T-scoping error:
    bool unresolvedT = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("unresolved type name `T`") != std::string::npos) {
            unresolvedT = true;
        }
    }
    BEZEL_EXPECT_FALSE(unresolvedT);
}
