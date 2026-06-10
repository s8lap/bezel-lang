// Sema tests for Shared<T> / Weak<T> instantiation rules
// (issue #603 / spec §28.13 / stdlib §2.16).
//
// §28.13: T in Shared<T> must be a class type (not a primitive, record, enum,
// interface, or function type). This constraint is checked at the sema layer
// (ExprTypecheck_Exprs.cpp, NewExpr branch for "Shared"/"Weak").
//
// Tests:
//   1. Shared<PrimitiveClass> is accepted (T is a class).
//   2. Shared<int> is rejected (T is a primitive).
//   3. Shared<string> is rejected (T is a primitive/string).
//   4. Shared<bool> is rejected.
//   5. Shared<IFace> is rejected (T is an interface, not a concrete class).
//   6. A collection literal passed to new Shared<T>([...]) is rejected
//      (#599 rule applied to Shared).
//
// Each test cites the spec section it verifies.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

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

// ──────────────────────────────────────────────────────────────────────────────
// §28.13 / stdlib §2.16: valid instantiation — T is a class.
// ──────────────────────────────────────────────────────────────────────────────

// §28.13 (must-pass): Shared<T> where T is a user-defined class is valid.
BEZEL_TEST(sema_shared_weak_instantiation_603,
           spec_28_13_shared_of_class_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class Inner end Inner; "
        "class C "
        "  public static method M() "
        "    var obj := new Inner(); "
        "    using var h := new Shared<Inner>(obj); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ──────────────────────────────────────────────────────────────────────────────
// §28.13 / stdlib §2.16: invalid instantiation — T is a primitive.
// ──────────────────────────────────────────────────────────────────────────────

// §28.13 (must-fail): Shared<int> — T is a primitive, not a class.
BEZEL_TEST(sema_shared_weak_instantiation_603,
           spec_28_13_shared_of_int_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M() "
        "    using var h := new Shared<int>(0); "
        "  end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.13 (must-fail): Shared<string> — T is a string primitive, not a class.
BEZEL_TEST(sema_shared_weak_instantiation_603,
           spec_28_13_shared_of_string_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M() "
        "    using var h := new Shared<string>(\"hi\"); "
        "  end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.13 (must-fail): Shared<bool> — T is a boolean primitive, not a class.
BEZEL_TEST(sema_shared_weak_instantiation_603,
           spec_28_13_shared_of_bool_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M() "
        "    using var h := new Shared<bool>(true); "
        "  end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.13 (must-fail): Shared<IFace> — T is an interface, not a concrete class.
BEZEL_TEST(sema_shared_weak_instantiation_603,
           spec_28_13_shared_of_interface_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface IFace "
        "  method Foo(): int; "
        "end IFace; "
        "class C "
        "  public static method M() "
        "    using var h := new Shared<IFace>(null); "
        "  end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ──────────────────────────────────────────────────────────────────────────────
// §15.4 / §28.13 (#599 applied to Shared): collection literal in
// new Shared<T>([...]) is rejected.
// ──────────────────────────────────────────────────────────────────────────────

// §15.4 / §28.13 (must-fail): collection literal passed to new Shared<T> —
// the constructor takes an owned T, not a collection literal.
BEZEL_TEST(sema_shared_weak_instantiation_603,
           spec_15_4_shared_collection_literal_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Inner end Inner; "
        "class C "
        "  public static method M() "
        "    using var h := new Shared<Inner>([new Inner()]); "
        "  end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
