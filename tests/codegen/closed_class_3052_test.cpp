// Codegen end-to-end for the §11.7 `closed` modifier (spec v0.242 — proposal
// 0086 item 3, tracked as #3052). `closed` is the dedicated spelling of what
// through v0.241 was the modifier pair `sealed abstract`:
//
//   §11.7, verbatim: "A class declared with the **`closed`** modifier is a
//   **closed abstract class**: a class that cannot be instantiated directly
//   and cannot be inherited from outside the declaring module. Classes derived
//   from a `closed` base must be declared in the same module; the compiler
//   rejects subclasses in other modules. A `closed` class may declare abstract
//   members (§12.5) without carrying the `abstract` modifier — `closed`
//   subsumes it."
//
// These tests AOT-compile and RUN the §11.7 example form and assert the
// produced exit code, proving the program compiles, links, and runs — the
// same shape as the retired-spelling sibling in sealed_abstract_test.cpp.
// Without the fix `closed` is an ordinary identifier and every program here
// fails to parse.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §11.7 (v0.242): `closed class Shape` with a same-module
// `sealed class Circle : Shape`. Circle(2).Area() = 3 * 2 * 2 = 12.
// The same-module subclass must be ACCEPTED — a `closed` base is inheritable
// within its declaring module, exactly as the retired pair was.
BEZEL_TEST(codegen_closed_class_3052, closed_hierarchy_same_module_runs_spec_11_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "closed class Shape "
        "  public abstract method Area(): int; "
        "end Shape; "
        "sealed class Circle : Shape "
        "  public R: int; "
        "  public constructor(r: int) R := r; end constructor; "
        "  public override method Area(): int => 3 * R * R; "
        "  destructor end destructor; "
        "end Circle; "
        "class C "
        "  public static method Main(): int "
        "    using var c := new Circle(2); "
        "    return c.Area(); "
        "  end Main; "
        "end C;"), 12);
}

// §11.7 / §12.5: "A `closed` class may declare abstract members (§12.5)
// without carrying the `abstract` modifier — `closed` subsumes it." The class
// below carries NO `abstract` modifier and declares an abstract method;
// §12.5's "a class containing one or more abstract members must itself be
// `abstract` (or `closed`)" must not reject it. Dispatch through the closed
// base type proves it is a usable polymorphic base, not merely declarable.
BEZEL_TEST(codegen_closed_class_3052, dispatch_through_closed_base_spec_12_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "closed class Shape "
        "  public abstract method Area(): int; "
        "end Shape; "
        "sealed class Square : Shape "
        "  public S: int; "
        "  public constructor(s: int) S := s; end constructor; "
        "  public override method Area(): int => S * S; "
        "  destructor end destructor; "
        "end Square; "
        "class C "
        "  public static method Main(): int "
        "    using var sq := new Square(4); "
        "    var sh: Shape := sq; "
        "    return sh.Area(); "
        "  end Main; "
        "end C;"), 16);
}

// §19.6 (v0.242) — the leaf-type definition reaches `closed`: "A **leaf type**
// is every **concrete** (neither `abstract` nor `closed`) class in the closed
// set … Abstract and `closed` members of the set have no instances (§11.7,
// §12.5) and need no arm."
//
// `Shape` is the `closed` root; `Rounded` is a `closed` INTERMEDIATE (no
// instances, no arm required); `Circle` and `Ring` are the two concrete
// leaves. Covering exactly the two leaves must both (a) compile with no `_`
// arm and (b) select the right arm at run time. Circle → 7, Ring → 9.
//
// The statement-position spelling additionally exercises §21.7's totality
// grant: `Pick` ends with the `case` and carries no trailing `return`, which
// is legal exactly because §19.6 proves the match exhaustive over the closed
// value space (§21.7 condition (i), sealed-hierarchy bullet).
BEZEL_TEST(codegen_closed_class_3052, closed_intermediate_is_not_a_leaf_spec_19_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "closed class Shape "
        "  public abstract method Tag(): int; "
        "end Shape; "
        "closed class Rounded : Shape "
        "end Rounded; "
        "sealed class Circle : Rounded "
        "  public override method Tag(): int => 7; "
        "  destructor end destructor; "
        "end Circle; "
        "sealed class Ring : Rounded "
        "  public override method Tag(): int => 9; "
        "  destructor end destructor; "
        "end Ring; "
        "class C "
        "  public static method Pick(s: Shape): int "
        "    case s of "
        "      var c: Circle => return c.Tag();, "
        "      var r: Ring => return r.Tag(); "
        "    end; "
        "  end Pick; "
        "  public static method Main(): int "
        "    using var ring := new Ring(); "
        "    return Pick(ring); "
        "  end Main; "
        "end C;"), 9);
}

// §12.4 control (v0.242): `sealed` ALONE keeps its own meaning — a final class
// that admits no subclass. The `closed` respell must not disturb it.
BEZEL_TEST(codegen_closed_class_3052, sealed_alone_still_final_spec_12_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "sealed class Point "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  destructor end destructor; "
        "end Point; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new Point(5); "
        "    return p.X; "
        "  end Main; "
        "end C;"), 5);
}

// §12.5 control (v0.242): `abstract` ALONE keeps its own meaning — a class
// that permits abstract members and forbids instantiation, with an OPEN
// subclass set. The `closed` respell must not disturb it.
BEZEL_TEST(codegen_closed_class_3052, abstract_alone_still_valid_spec_12_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Animal "
        "  public abstract method Legs(): int; "
        "end Animal; "
        "class Dog : Animal "
        "  public override method Legs(): int => 4; "
        "  destructor end destructor; "
        "end Dog; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dog(); "
        "    var a: Animal := d; "
        "    return a.Legs(); "
        "  end Main; "
        "end C;"), 4);
}
