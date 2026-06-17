// Spec-derived sema tests for fix #747 (R42 FALSE_ACCEPT) — assignment to a
// `const` member.
//
// Authority: §10 / §22 — a `const` member is a compile-time constant; it is
// initialized by its declaration and is never assignable. Bug: the
// readonly-assignment check only handled a `this.<field>` target carrying the
// `readonly` modifier, so an assignment to a `const` member was silently
// accepted (compiled, dropped at runtime). The fix rejects assignment to a
// const member through any receiver shape (`this.K`, `c.K`, `Type.K`, bare `K`)
// and in any context, including a constructor.

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

bool hasConstAssignError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("cannot assign to const member") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §10: assignment through an instance receiver `c.K` is rejected.
BEZEL_TEST(const_assignment_747, instance_receiver_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public const K: int := 100; "
        "  public static method Main(): int "
        "    using var c := new C(); c.K := 5; return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasConstAssignError(r));
}

// §10: assignment through `this.K` inside a method is rejected.
BEZEL_TEST(const_assignment_747, this_receiver_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public const K: int := 100; "
        "  public method M() this.K := 5; end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasConstAssignError(r));
}

// §10: a type-qualified static const `C.K` is rejected.
BEZEL_TEST(const_assignment_747, static_qualified_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static const K: int := 100; "
        "  public static method Main(): int C.K := 5; return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasConstAssignError(r));
}

// §10: a const is never assignable — not even in a constructor.
BEZEL_TEST(const_assignment_747, in_constructor_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public const K: int := 100; "
        "  public constructor() K := 5; end constructor; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasConstAssignError(r));
}

// §10: reading a const (no assignment) is fine — no error.
BEZEL_TEST(const_assignment_747, reading_const_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public const K: int := 100; "
        "  public static method Main(): int return C.K; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasConstAssignError(r));
}

namespace {
bool hasConstInitError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("must be initialized with a compile-time-constant") !=
                std::string::npos)
            return true;
    return false;
}
}  // namespace

// §22.12 (#747 R42): a const initialized from a (non-const) static field is
// NOT a compile-time constant — rejected. Pre-fix it was silently accepted and
// treated like a static-field initializer.
BEZEL_TEST(const_assignment_747, nonconst_static_field_initializer_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static _runtime: int := 5; "
        "  public const K: int := _runtime * 2; "
        "  public static method Main(): int return K; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasConstInitError(r));
}

// §22.12: a const initialized by a method call is not constant — rejected.
BEZEL_TEST(const_assignment_747, method_call_initializer_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Compute(): int return 7; end Compute; "
        "  public const K: int := Compute(); "
        "  public static method Main(): int return K; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasConstInitError(r));
}

// §22.12 regression guard: literals, operations over constants, references to
// other consts, and conditionals over constants are all admitted.
BEZEL_TEST(const_assignment_747, constant_expressions_accepted) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public const A: int := 40; "
        "  public const B: int := A * 2 + 1; "
        "  public const S: string := \"hi\"; "
        "  public const F: bool := true; "
        "  public const G: int := A if F else B; "
        "  public static method Main(): int return A + B; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasConstInitError(r));
}
