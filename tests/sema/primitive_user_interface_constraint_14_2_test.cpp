// Spec-derived regression tests for §14.2 "Interface conformance" clause 5
// (v0.196): a PRIMITIVE type argument conforms ONLY to the interfaces its
// standard-library declaration lists (IComparable<self>/IEquatable<self>,
// stdlib §9) plus the §11.8 compiler-provided static abstractions
// (INumber/ISteppable/IParsable, char subset) — every one of which is a
// compiler-known builtin interface. A primitive can NEVER conform to a
// USER-declared interface: primitives cannot declare conformances.
//
// The defect this pins: `M<int>()` under `where T: IShape` (an ordinary,
// instance-only user interface) was SILENTLY ACCEPTED at both constraint-check
// sites — the method-call site (ExprTypecheck_Calls.cpp, the primitive `argDecl
// == null` branch) and the type-instantiation site (Resolver::checkOneConstraint
// in TypeResolution_Checks.cpp). Both gated the reject on whether the interface
// carried STATIC members, so an instance-only user interface fell through to a
// defer/accept. Codegen then emitted an itable dispatch on a boxed `int` that
// has no such itable — an unsound program the type system should have rejected
// (the #729 class of hole). Fix: reject when the interface bound resolves to any
// declaration other than a synthesized builtin interface (bound->declaration !=
// ctx_.builtinInterfaceSym(name)).
//
// Authority: spec §14.2 clause 5 (v0.196), stdlib §9 / §2.18 / §2.19.

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff a `where T: …` constraint-not-satisfied diagnostic was reported.
bool hasConstraintError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not satisfy the constraint") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── REJECT: primitive vs a plain (instance-only) USER interface ─────────────
// These are the fix-critical cases — accepted before the fix, rejected after.

// §14.2 clause 5: `int` cannot conform to the user interface `IShape`; the
// EXPLICIT method-call type argument must be rejected.
BEZEL_TEST(primitive_user_iface_constraint_14_2,
           callsite_int_vs_user_interface_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "static class F "
        "  public static method Gen<T>(v: T): int where T: IShape return 0; "
        "  end Gen; "
        "end F; "
        "class C public static method Main(): int "
        "  return F.Gen<int>(5); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2 clause 5: the same rejection at the generic-TYPE instantiation site
// (`new Box<int>()` where `Box<T> where T: IShape`).
BEZEL_TEST(primitive_user_iface_constraint_14_2,
           typeinst_int_vs_user_interface_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Box<T> where T: IShape public _v: T?; end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2 clause 5: a user interface that DOES carry a static member is likewise
// unconformable by a primitive (this case was already rejected pre-fix via the
// static-member gate; kept so the fix does not silently regress it).
BEZEL_TEST(primitive_user_iface_constraint_14_2,
           callsite_int_vs_user_static_interface_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface IHasStatic static method Make(): int; end IHasStatic; "
        "static class F "
        "  public static method Gen<T>(v: T): int where T: IHasStatic return 0; "
        "  end Gen; "
        "end F; "
        "class C public static method Main(): int "
        "  return F.Gen<int>(5); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2 clause 5: the unsound program no longer compiles end-to-end through the
// REAL AOT driver — the body dispatches `v.Area()` on a boxed `int`.
BEZEL_TEST(primitive_user_iface_constraint_14_2,
           callsite_int_dispatch_rejected_by_aot) {
    auto r = runAot(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "static class F "
        "  public static method Gen<T>(v: T): int where T: IShape "
        "    return v.Area(); "
        "  end Gen; "
        "end F; "
        "class C public static method Main(): int "
        "  return F.Gen<int>(5); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran && r.diagnostics.hasErrors());
}

// ── ACCEPT: legitimate conformances must be UNAFFECTED (no false positives) ──

// §14.2 worked example `where T: IComparable<int>`: `int` conforms through its
// stdlib declaration (stdlib §9.1). Must stay accepted.
BEZEL_TEST(primitive_user_iface_constraint_14_2,
           callsite_int_vs_icomparable_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "static class F "
        "  public static method Gen<T>(v: T): int where T: IComparable<int> "
        "    return 0; "
        "  end Gen; "
        "end F; "
        "class C public static method Main(): int "
        "  return F.Gen<int>(5); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// §11.8 / stdlib §2.19: `int` conforms to the compiler-provided `INumber<T>`.
// Must stay accepted.
BEZEL_TEST(primitive_user_iface_constraint_14_2,
           callsite_int_vs_inumber_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "static class F "
        "  public static method Gen<T>(v: T): int where T: INumber<T> "
        "    return 0; "
        "  end Gen; "
        "end F; "
        "class C public static method Main(): int "
        "  return F.Gen<int>(5); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// §9.6: `string` conforms to `IEnumerable<char>` through its stdlib declaration.
// The name-recognized builtin interface carries no synthesized declaration, so
// the primitive stays on the conservative defer path — must NOT be rejected.
BEZEL_TEST(primitive_user_iface_constraint_14_2,
           callsite_string_vs_ienumerable_char_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "static class F "
        "  public static method Gen<T>(v: T): int where T: IEnumerable<char> "
        "    return 0; "
        "  end Gen; "
        "end F; "
        "class C public static method Main(): int "
        "  return F.Gen<string>(\"hi\"); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// §14.2 clause 1: a user class that DOES implement the interface stays accepted.
BEZEL_TEST(primitive_user_iface_constraint_14_2,
           callsite_conforming_user_class_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Square: IShape public method Area(): int => 4; end Square; "
        "static class F "
        "  public static method Gen<T>(v: T): int where T: IShape "
        "    return v.Area(); "
        "  end Gen; "
        "end F; "
        "class C public static method Main(): int "
        "  var s := new Square(); "
        "  return F.Gen<Square>(s); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}
