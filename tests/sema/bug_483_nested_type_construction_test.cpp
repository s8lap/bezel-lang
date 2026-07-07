// Spec-derived sema tests for bug #483 — `new Outer.Inner(args)` must resolve
// the full QualifiedName as a nested type, not stop at the first component.
//
// Authority: Grammar A.8 / §11.3 (nested class declarations).
// NewExpr ::= "new" Type "(" ArgList? ")"
// Type → TypeNoNullable → QualifiedName GenericArgs?
// QualifiedName ::= Identifier ("." Identifier)*
//
// When `Outer.Inner` is used as a type name in `new`, sema must descend
// through `Outer`'s member list to find the nested `TypeSymbol` for `Inner`
// and attach it as the resolved type declaration. Using only the first part
// (`Outer`) was the pre-fix behaviour.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::NestedTypeSymbol;
using bezel::sema::TypeRef;
using bezel::sema::TypeSymbol;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Helper: look up the TypeSymbol registered for a name in the module's
// children.
const TypeSymbol* findType(const bezel::test::SemaResult& r,
                           std::string_view name) {
    if (!r.ctx || !r.ctx->module()) return nullptr;
    for (auto* child : r.ctx->module()->children) {
        if (auto* ts = dynamic_cast<TypeSymbol*>(child)) {
            if (ts->name == name) return ts;
        }
    }
    return nullptr;
}

// Helper: find a NestedTypeSymbol in a TypeSymbol's member list.
const TypeSymbol* findNestedType(const TypeSymbol* outer,
                                 std::string_view name) {
    if (!outer) return nullptr;
    for (auto* m : outer->members) {
        if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
            if (nt->name == name && nt->type) return nt->type;
        }
    }
    return nullptr;
}

} // namespace

// §11.3 / Grammar A.8: `new Outer.Inner()` where Inner has the implicit
// default constructor — sema must NOT diagnose an error; the type resolves
// to Inner, not Outer.
BEZEL_TEST(sema_bug_483, nested_type_construction_no_error_spec_11_3) {
    auto r = parseSemaAll(
        "module M; "
        "class Outer "
        "  public class Inner "
        "    public _v: int := 99; "
        "    destructor end destructor; "
        "  end Inner; "
        "end Outer; "
        "class Main2 "
        "  public static method Main(): int "
        "    var i := new Outer.Inner(); "
        "    return i._v; "
        "  end Main; "
        "end Main2;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.3: `new Outer.Inner(args)` with an explicit constructor in Inner.
BEZEL_TEST(sema_bug_483, nested_type_construction_with_ctor_args_spec_11_3) {
    auto r = parseSemaAll(
        "module M; "
        "class Outer "
        "  public class Inner "
        "    public _v: int := 0; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    destructor end destructor; "
        "  end Inner; "
        "end Outer; "
        "class C "
        "  public static method Main(): int "
        "    var i := new Outer.Inner(42); "
        "    return i._v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Regression: `new OuterOnly()` (single-identifier, no nesting) must still
// resolve correctly.
BEZEL_TEST(sema_bug_483, single_identifier_regression_spec_A8) {
    auto r = parseSemaAll(
        "module M; "
        "class Box "
        "  public _v: int := 0; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(); "
        "    return b._v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Regression: `new Generic<int>()` must still resolve correctly.
BEZEL_TEST(sema_bug_483, generic_type_regression_spec_A8) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  public _v: int := 0; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box<int>(); "
        "    return b._v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The inner TypeSymbol is the one sema should attach: not the outer one.
BEZEL_TEST(sema_bug_483, nested_type_resolves_to_inner_tsym_spec_11_3) {
    auto r = parseSemaAll(
        "module M; "
        "class Outer "
        "  public class Inner "
        "    public _v: int := 0; "
        "    destructor end destructor; "
        "  end Inner; "
        "end Outer; "
        "class C "
        "  public static method Main(): int "
        "    var i := new Outer.Inner(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // Confirm the symbol table has Outer with Inner as a nested member.
    const TypeSymbol* outer = findType(r, "Outer");
    BEZEL_EXPECT_TRUE(outer != nullptr);
    if (outer) {
        const TypeSymbol* inner = findNestedType(outer, "Inner");
        BEZEL_EXPECT_TRUE(inner != nullptr);
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.5 (#955): a NESTED record/class called WITHOUT `new` (`Outer.Point(1,2)`)
// is not construction and must be rejected with the same diagnostic as a
// top-level record/class — it previously slipped past (the NestedTypeSymbol
// callee misses the TypeSymbol-based §9.5 check) and miscompiled.
BEZEL_TEST(sema_bug_483, nested_record_without_new_rejected_955) {
    auto r = parseSemaAll(
        "module M; "
        "public class Outer public record Point(X: int, Y: int); end Outer; "
        "public class Program public static method Main(): int "
        "  var p := Outer.Point(10, 20); return p.X; "  // no `new` — rejected
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("constructed with `new") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §9.5: `new Outer.Point(...)` (the correct form) and a nested enum cast
// `Outer.E(1)` remain accepted (no over-rejection of the nested branch).
BEZEL_TEST(sema_bug_483, new_nested_record_and_nested_enum_cast_accepted_955) {
    auto r = parseSemaAll(
        "module M; "
        "public class Outer public record Point(X: int, Y: int); "
        // per §13.3/§11.6 (v0.181, #1558): nested types require explicit
        // visibility — no default.
        "  public enum E A, B; end E; end Outer; "
        "public class Program public static method Main(): int "
        "  var p := new Outer.Point(10, 20); var e: Outer.E := Outer.E(1); "
        "  return p.X + int(e); "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
