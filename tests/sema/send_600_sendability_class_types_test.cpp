// #600 contract — §30.14 sendable types, class-type "transfer-closed" rules.
//
// These tests translate §30.14 transfer-closed class sendability into sema
// diagnostics. They are EXPECTED TO FAIL against current main: sema today has
// no Channel<T>, Domain.Spawn, or sendable-property checking. The impl lane
// re-enables them from CMakeLists.txt.
//
// Spec derivation:
//   §30.14 "Class types are sendable iff the class is transfer-closed":
//     Rule 1: must be `sealed` (or `sealed abstract` with all-sendable hierarchy).
//     Rule 2: every instance field must be: sendable value type, string, owned
//       field of sendable class, or [N owned]C where C is sendable.
//     Rule 3 (implied): no function-typed, Object-typed, or interface-typed
//       fields; no non-owned class-typed fields (back-references).
//   §3.6: diagnostics name "not sendable".
//   §30.15: Channel<T> element type must be sendable.

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

bool hasNotSendableDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not sendable") != std::string::npos ||
            d.message.find("sendable") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Rule 1: sealed class — the basic acceptance case (§30.14 transfer-closed).
// ---------------------------------------------------------------------------

// §30.14 Rule 1+2: sealed class with only primitive and string fields → sendable.
// Channel<Report> with such a class must be accepted.
BEZEL_TEST(send_600_class_types,
           sealed_class_primitive_string_fields_accepted_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Report "
        "  public Id: int := 0; "
        "  public Name: string := \"\"; "
        "  destructor end destructor; "
        "end Report; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Report>(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14 Rule 1: open (non-sealed) class → not sendable.
// An open class could be subclassed by a type holding origin-domain refs.
BEZEL_TEST(send_600_class_types,
           open_class_as_channel_element_rejected_30_14_rule1) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenData "
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end OpenData; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<OpenData>(); "
        "  end Test; "
        "end C;");
    // Rejected: OpenData is not sealed → not transfer-closed → not sendable.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14 Rule 2: sealed class with a non-owned class-typed field (a
// back-reference / borrow alias) is not sendable.
BEZEL_TEST(send_600_class_types,
           sealed_class_with_non_owned_class_field_rejected_30_14_rule2) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Node "
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end Node; "
        // Back-reference: 'Parent' is a borrow (not owned) → violates rule 2.
        "sealed class Tree "
        "  public Parent: Node?; "   // non-owned class-typed field
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end Tree; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Tree>(); "
        "  end Test; "
        "end C;");
    // Rejected: Tree has a non-owned class-typed field → not sendable.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14 Rule 3: sealed class with a function-typed field → not sendable
// ("no function-typed fields").
BEZEL_TEST(send_600_class_types,
           sealed_class_with_function_field_rejected_30_14_rule3) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Handler "
        "  public Callback: () -> (); "   // function-typed field
        "  destructor end destructor; "
        "end Handler; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Handler>(); "
        "  end Test; "
        "end C;");
    // Rejected: function-typed field → not sendable.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14 Rule 3: sealed class with an interface-typed field → not sendable
// ("no interface-typed fields — boxed contents cannot be statically verified").
BEZEL_TEST(send_600_class_types,
           sealed_class_with_interface_field_rejected_30_14_rule3) {
    auto r = parseSemaAll(
        "module M; "
        "interface IWorker method Work(): int; end IWorker; "
        "sealed class Job "
        "  public owned Worker: IWorker?; "   // interface-typed field
        "  destructor end destructor; "
        "end Job; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Job>(); "
        "  end Test; "
        "end C;");
    // Rejected: interface-typed field → not sendable.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14 Rule 3: sealed class with an Object-typed field → not sendable
// ("no Object-typed fields — runtime types behind Object cannot be verified").
BEZEL_TEST(send_600_class_types,
           sealed_class_with_object_field_rejected_30_14_rule3) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Bag "
        "  public Payload: Object?; "   // Object-typed field
        "  destructor end destructor; "
        "end Bag; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Bag>(); "
        "  end Test; "
        "end C;");
    // Rejected: Object-typed field → not sendable.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14 Rule 2: sealed class with an `owned` field whose type is another
// sendable sealed class → sendable (nested ownership tree all moves together).
BEZEL_TEST(send_600_class_types,
           sealed_class_owned_sendable_field_accepted_30_14_rule2) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Inner "
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end Inner; "
        "sealed class Outer "
        "  public owned Child: Inner := new Inner(); "
        "  destructor end destructor; "
        "end Outer; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Outer>(); "
        "  end Test; "
        "end C;");
    // Accepted: Outer owns Inner (both sealed, no forbidden fields).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14 Rule 2: sealed class with an `owned` field whose type is a NON-sendable
// class (open inner) → the outer is also not sendable (transitivity).
BEZEL_TEST(send_600_class_types,
           sealed_class_with_owned_non_sendable_field_rejected_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenInner "   // open (not sealed) → not sendable
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end OpenInner; "
        "sealed class Outer "
        "  public owned Child: OpenInner := new OpenInner(); "
        "  destructor end destructor; "
        "end Outer; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Outer>(); "
        "  end Test; "
        "end C;");
    // Rejected: OpenInner not sendable → Outer not sendable (transitivity).
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14: `sealed abstract` + all-sendable closed hierarchy → the whole
// hierarchy is sendable (coinductive evaluation over the closed set).
BEZEL_TEST(send_600_class_types,
           sealed_abstract_all_sendable_hierarchy_accepted_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class Shape "
        "  public abstract method Area(): int; "
        "end Shape; "
        "sealed class Circle : Shape "
        "  public R: int := 0; "
        "  public override method Area(): int => 0; "
        "  destructor end destructor; "
        "end Circle; "
        "sealed class Rect : Shape "
        "  public W: int := 0; "
        "  public H: int := 0; "
        "  public override method Area(): int => 0; "
        "  destructor end destructor; "
        "end Rect; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Shape>(); "
        "  end Test; "
        "end C;");
    // Accepted: sealed abstract Shape with fully-sendable sealed subclasses.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14: `sealed abstract` hierarchy where ONE subclass is not sendable
// (has a non-owned class field) → the whole hierarchy is not sendable.
BEZEL_TEST(send_600_class_types,
           sealed_abstract_one_non_sendable_subclass_rejected_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class Shape "
        "  public abstract method Area(): int; "
        "end Shape; "
        "sealed class GoodCircle : Shape "
        "  public R: int := 0; "
        "  public override method Area(): int => 0; "
        "  destructor end destructor; "
        "end GoodCircle; "
        "class ExternalRef end ExternalRef; "
        // BadRect has a non-owned class-typed field → breaks the hierarchy.
        "sealed class BadRect : Shape "
        "  public W: int := 0; "
        "  public Ref: ExternalRef?; "   // non-owned class-typed field
        "  public override method Area(): int => 0; "
        "  destructor end destructor; "
        "end BadRect; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Shape>(); "
        "  end Test; "
        "end C;");
    // Rejected: BadRect breaks the hierarchy's sendability.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14: Mutually-recursive owned sendable classes — coinductive rule admits
// them. A <-> B where both are sealed with only owned fields of each other.
// Per spec: "Mutually-recursive owned references among sendable classes are
// admitted; the property is evaluated coinductively."
BEZEL_TEST(send_600_class_types,
           mutually_recursive_owned_sendable_classes_accepted_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class NodeA "
        "  public Val: int := 0; "
        "  public owned Next: NodeB?; "
        "  destructor end destructor; "
        "end NodeA; "
        "sealed class NodeB "
        "  public Val: int := 0; "
        "  public owned Prev: NodeA?; "
        "  destructor end destructor; "
        "end NodeB; "
        "class C "
        "  public static method Test() "
        "    using var chA := new Channel<NodeA>(); "
        "    using var chB := new Channel<NodeB>(); "
        "  end Test; "
        "end C;");
    // Accepted: coinductive evaluation of the mutual recursion.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
