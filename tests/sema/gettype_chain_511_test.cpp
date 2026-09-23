// Tests for issue #511: GetType()/typeof chaining causes silent wrong-result,
// SIGSEGV, or IR verification failure.
//
// Root cause: fix #487 (commit 651b8e5) emitted the "reflection not yet
// implemented" guard only in CODEGEN.  When the call was chained (e.g.
// `obj.GetType().Name`) or used inside a lambda or generic method body, the
// codegen guard was bypassed — the inner GetType call produced a poison value
// that was silently propagated, crashing or silently miscompiling.
//
// Fix #511 moves the guard to SEMA so that every use of GetType() / typeof,
// regardless of context, produces a clean compile error before codegen runs.
// The expression is still TYPED as the `Type` builtin class (stdlib §2.13)
// so that no secondary "expression has no type" cascades are emitted and the
// future-implementation path (System.Reflection §11) is preserved.
//
// Spec basis:
//   §8.1   Object universal members — GetType is present on every type.
//   §8.2   v.GetType() → Type   (runtime type descriptor)
//   §17.10 typeof(T) → Type
//   §11    System.Reflection (deferred; this is what makes GetType/typeof NYI)
//   stdlib §2.13  The `Type` sealed class
//
// Each BEZEL_TEST here derives from one of the three confirmed bugs or a
// related variant.  Tests are labelled by bug number and spec section.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// Run the full sema pipeline on the given source.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff any diagnostic message contains the given substring.
bool anyDiagContains(const bezel::test::SemaResult& r,
                     std::string_view substr) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(std::string(substr)) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// ===========================================================================
// Bug 1 (HIGH, silent wrong-result): `var name := obj.GetType().Name;`
// compiles silently (exit 0) and runs wrong on base 848e2d0.
// The outer MemberAccessExpr `.Name` is lowered; the inner GetType guard
// fires in codegen but is bypassed by the chaining.
// After fix #511: sema emits the reflection error on the inner CallExpr,
// errorCount > 0, driver exits non-zero, no binary.
// §8.2 / §11
// ===========================================================================

// §8.2: `obj.GetType().Name` — chained member access on GetType result.
// Must produce a clean sema diagnostic, NOT compile silently.  (Bug 1)
BEZEL_TEST(sema_gettype_chain_511, gettype_chain_name_diagnosed_8_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public _id: int; end Widget; "
        "class C "
        "  public method M(w: Widget): string "
        "    var name := w.GetType().Name; "
        "    return name; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "reflection"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "not yet implemented"));
}

// §8.2: `this.GetType().FullName` — chained on this-receiver.
BEZEL_TEST(sema_gettype_chain_511, gettype_chain_this_diagnosed_8_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(): string "
        "    var fn := this.GetType().FullName; "
        "    return fn; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "reflection"));
}

// §8.2: `GetType()` result passed as a call argument.
BEZEL_TEST(sema_gettype_chain_511, gettype_as_argument_diagnosed_8_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Inspector "
        "  public static method Inspect(t: Type): string => \"\"; "
        "end Inspector; "
        "class C "
        "  public method M(w: Inspector): string "
        "    return Inspector.Inspect(w.GetType()); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "reflection"));
}

// ===========================================================================
// Bug 2 (CRITICAL, SIGSEGV): a generic class body using `_v.GetType().Name`
// caused a runtime SIGSEGV on base 848e2d0.
// After fix #511: sema rejects at the GetType CallExpr inside the body;
// errorCount > 0, no binary, no SIGSEGV.
// §8.2 / §14 / §11
// ===========================================================================

// §8.2 / §14: generic method body calling GetType() on T-typed field.
// On base 848e2d0 this compiled and then segfaulted at runtime.  (Bug 2)
BEZEL_TEST(sema_gettype_chain_511, generic_body_gettype_diagnosed_8_2_14) {
    auto r = parseSemaAll(
        "module M; "
        "class G<T> "
        "  public _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  destructor end destructor; "
        "  public method GetTypeName(): string "
        "    return _v.GetType().Name; "
        "  end GetTypeName; "
        "end G; "
        "class C "
        "  public static method Main(): int "
        "    using var g := new G<int>(42); "
        "    var s := g.GetTypeName(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "reflection"));
}

// ===========================================================================
// Bug 3 (CRITICAL, IR-fail): `var f := () => p.GetType().Name;` inside a
// method caused LLVM IR verification failure on base 848e2d0 (ret void / i32).
// After fix #511: sema emits the reflection error at the GetType node inside
// the lambda body; errorCount > 0, codegen never runs, no IR failure.
// §8.2 / §17.5 (lambda) / §11
// ===========================================================================

// §8.2 / §17.5: GetType() inside a lambda expression body — IR-fail bug.
// On base 848e2d0 this produced an LLVM IR verification error.  (Bug 3)
BEZEL_TEST(sema_gettype_chain_511, lambda_gettype_diagnosed_8_2_17_5) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(x: int, y: int); "
        "class C "
        "  public method M(p: Point) "
        "    var f := lambda() => p.GetType().Name; "
        "    _ := f; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "reflection"));
}

// ===========================================================================
// typeof chain — same root cause as Bug 1 but with typeof(T).Name.
// §17.10 / §11
// ===========================================================================

// §17.10: `typeof(int).Name` — chained member access on typeof result.
// The sema diagnostic must fire on the TypeOfExpr itself.
BEZEL_TEST(sema_gettype_chain_511, typeof_chain_diagnosed_17_10) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(): string "
        "    var name := typeof(int).Name; "
        "    return name; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "reflection"));
}

// §17.10: `typeof(MyClass).FullName` on a user-declared class.
BEZEL_TEST(sema_gettype_chain_511, typeof_user_type_chain_diagnosed_17_10) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public _x: int; end Foo; "
        "class C "
        "  public method M(): string "
        "    var fn := typeof(Foo).FullName; "
        "    return fn; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "reflection"));
}

// ===========================================================================
// GetType() on record and enum (§8.2 — valid on all types; must diagnose)
// ===========================================================================

// §8.2: GetType() on a record-typed receiver — must diagnose.
BEZEL_TEST(sema_gettype_chain_511, gettype_on_record_chain_diagnosed_8_2) {
    auto r = parseSemaAll(
        "module M; "
        "record Size(w: int, h: int); "
        "class C "
        "  public method M(s: Size): string "
        "    var n := s.GetType().Name; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "reflection"));
}

// §8.2: GetType() on an enum-typed receiver — must diagnose.
BEZEL_TEST(sema_gettype_chain_511, gettype_on_enum_chain_diagnosed_8_2) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(c: Color): string "
        "    var n := c.GetType().Name; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "reflection"));
}

// ===========================================================================
// Bare GetType() (no chain) still diagnosed — regression guard for #487.
// The base GetType() case that #487 used to expose via codegen must still
// produce a clean diagnostic after #511 moves it to sema.
// ===========================================================================

// §8.2: basic `var t := obj.GetType()` still diagnosed (regression for #487).
BEZEL_TEST(sema_gettype_chain_511, bare_gettype_still_diagnosed_8_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Node public _val: int; end Node; "
        "class C "
        "  public method M(n: Node) "
        "    var t := n.GetType(); "
        "    _ := t; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "GetType"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "not yet implemented"));
}

// §17.10: basic `var t := typeof(int)` still diagnosed (regression for #487).
BEZEL_TEST(sema_gettype_chain_511, bare_typeof_still_diagnosed_17_10) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M() "
        "    var t := typeof(int); "
        "    _ := t; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "typeof"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "not yet implemented"));
}

// ===========================================================================
// CONTROL — normal member chains still work.
// A regular method chain (no GetType/typeof) must NOT be rejected.
// §11.3 / §12.1
// ===========================================================================

// §11.3 / §12.1: normal member chain (no reflection) still compiles cleanly.
BEZEL_TEST(sema_gettype_chain_511, normal_member_chain_unaffected) {
    auto r = parseSemaAll(
        "module M; "
        "class Name "
        "  public _first: string; "
        "  public constructor(f: string) _first := f; end constructor; "
        "  destructor end destructor; "
        "  public method First(): string => _first; "
        "end Name; "
        "class Builder "
        // `owned` is REQUIRED here, not decoration (#4758). Per §28.3
        // announcement table row 4 (v0.307), a plain concrete class-typed
        // instance return admits only a receiver-sourced borrow, and
        // `new Name("Alice")` is a fresh owned value. This row runs on
        // `parseSemaAll` (sema only, no ownership pass), so the unmarked
        // spelling would surface only against the full driver.
        "  public method MakeName(): owned Name => new Name(\"Alice\"); "
        "  destructor end destructor; "
        "end Builder; "
        "class C "
        "  public method M(b: Builder): string "
        "    using var n := b.MakeName(); "
        "    return n.First(); "
        "  end M; "
        "end C;");
    // A non-reflection chain must not produce the reflection diagnostic.
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "reflection"));
}
