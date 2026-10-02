// fix #5250 (§3.4, v0.313): `Object` is a class — the root class of the
// type system per spec §3.4 ("Object has no lowercase keyword alias…Object
// is a class, named Object or, qualified, System.Object"), not a primitive
// value type. Two code paths misclassified it as a non-class.
//
// PATH 1 — `owningElementBadDesc` (TypeResolution_Checks.cpp):
//   `PrimitiveType::Object` fell into the `Kind::Primitive` arm and
//   returned "a primitive value type", causing Shared<T>/Weak<T>/etc.
//   to reject Object as the type argument at the §14.2 constraint check.
//   Fix: add an explicit `PrimitiveType::Object` case that returns {}
//   (admitted) — Object is a class and satisfies `where T: class`.
//
// PATH 2 — Shared/Weak instantiation check (ExprTypecheck_Exprs.cpp):
//   The comment at line 5035 listed "Object (primitive)" and excluded
//   Object from the "builtin named types" branch, causing Object to fall
//   through to the non-class diagnostic path. Fix: update the comment
//   and add `PrimitiveType::Object` to the "admitted" check.
//
// Spec citations:
//   §3.4 (v0.313): "Object is a class" — the root class, no lowercase alias
//   §14.2 (v0.313): where T: class — non-nullable reference types only
//   §14.4 (v0.313): class-only restriction on owning generics
//   stdlib §2.16: public class Shared<T>: IDisposable where T: class
//
// Regression: shared_object_class_accept_5250

#include "support/aot_support.hpp"         // runAot
#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

// Shared<Object> must ACCEPT — Object is a class per §3.4 and satisfies
// Shared<T>'s `where T: class` constraint per stdlib §2.16.
void expectSharedObjectAccepted(const std::string& snippet) {
    auto r = runAot(
        "module M5250; "
        "class C5250 "
        "  public static method Main(): int "
        + snippet +
        "  end Main; "
        "end C5250;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Shared<int> must STILL REJECT — int is a primitive value type, not a
// class, and does not satisfy Shared<T>'s `where T: class` constraint.
void expectSharedIntRejected() {
    auto r = runAot(
        "module M5250i; "
        "class C5250i "
        "  public static method Main(): int "
        "    var x: int := 0; "
        "    var s := new Shared<int>(move x); "
        "    return 0; "
        "  end Main; "
        "end C5250i;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "Shared<T> requires T to be a proper class type"));
}

// Shared<Widget> where Widget is a user class must ACCEPT — control.
void expectSharedUserClassAccepted() {
    auto r = runAot(
        "module M5250w; "
        "class Widget5250 "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Widget5250; "
        "class Prog5250w "
        "  public static method Main(): int "
        "    var o := new Widget5250(42); "
        "    using var s := new Shared<Widget5250>(move o); "
        "    return s.Get().N; "
        "  end Main; "
        "end Prog5250w;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

}  // namespace

// ── (1) PRIMARY FIX: Shared<Object> via new — must ACCEPT ───────────────
BEZEL_TEST(shared_object_class_accept_5250,
           shared_object_via_new_accepts_per_spec_3_4) {
    expectSharedObjectAccepted(
        "var o := new Object(); "
        "using var s := new Shared<Object>(move o); "
        "return 0;");
}

// ── (2) PRIMARY FIX: Shared<Object> via default — must ACCEPT ──────────
BEZEL_TEST(shared_object_class_accept_5250,
           shared_object_via_default_accepts_per_spec_3_4) {
    expectSharedObjectAccepted(
        "var s: Shared<Object>? := null; "
        "return 0;");
}

// ── (3) CONTROL: Shared<int> must still REJECT (value type) ───────────
BEZEL_TEST(shared_object_class_accept_5250,
           shared_int_still_rejects_primitive_value_type) {
    expectSharedIntRejected();
}

// ── (4) CONTROL: Shared<Widget> for a user class must still ACCEPT ─────
BEZEL_TEST(shared_object_class_accept_5250,
           shared_user_class_still_accepts_control) {
    expectSharedUserClassAccepted();
}

// ── (5) PRIMARY FIX: Weak<Object> via Downgrade() — must ACCEPT ────────
// Weak<T> has no direct constructor; obtain via Shared<T>.Downgrade().
BEZEL_TEST(shared_object_class_accept_5250,
           weak_object_via_shared_downgrade_accepts_per_spec_3_4) {
    auto r = runAot(
        "module M5250w; "
        "class C5250w "
        "  public static method Main(): int "
        "    var o := new Object(); "
        "    using var sh := new Shared<Object>(move o); "
        "    using var w := sh.Downgrade(); "
        "    return 0; "
        "  end Main; "
        "end C5250w;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (6) PRIMARY FIX: Weak<Object> via default — must ACCEPT ───────────
BEZEL_TEST(shared_object_class_accept_5250,
           weak_object_via_default_accepts_per_spec_3_4) {
    auto r = runAot(
        "module M5250w2; "
        "class C5250w2 "
        "  public static method Main(): int "
        "    var w: Weak<Object>? := null; "
        "    return 0; "
        "  end Main; "
        "end C5250w2;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
