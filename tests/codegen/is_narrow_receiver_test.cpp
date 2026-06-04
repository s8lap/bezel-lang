// Codegen JIT tests for §12.7 flow narrowing — receiver materialization.
//
// Per §12.7 (Type tests: is, is not, as): after `if x is T` narrows a simple
// variable to T within the controlled block, a member access on that variable
// resolves against T and must run on the *actual* object. Before §12.7
// narrowing was implemented, sema never resolved `x.MemberOnlyOnT`, so codegen
// got a null callee and lowered the access to a garbage/zero receiver:
//   * `if animal is Dog then return animal.Bark(); end` returned 0/garbage
//     instead of 100,
//   * `return animal._tag;` returned 0 instead of 77.
// These derive from issue #413's repros (/tmp/disc/is-narrow). The observed
// value is load-bearing: it was garbage/0 before the fix and is the spec
// result after.
//
// The last test is a regression guard: a *virtual* method declared on the
// base, invoked after narrowing, must still dispatch correctly through the
// base-valid `this` (it did before; narrowing must not break it).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §12.7 canonical example: derived-only method on an is-narrowed variable.
// `Bark` is declared only on Dog; `Bark() => _tag + 23` so it READS the
// receiver (proving the right object pointer is used, not a garbage one).
BEZEL_TEST(codegen_isnarrow, derived_only_method_reads_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal public virtual method Legs(): int => 0; end Animal; "
        "class Dog : Animal "
        "  public _tag: int; "
        "  public constructor() _tag := 77; end constructor; "
        "  public override method Legs(): int => 4; "
        "  public method Bark(): int => _tag + 23; "
        "end Dog; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dog(); "
        "    var animal: Animal := d; "
        "    if animal is Dog then "
        "      return animal.Bark(); "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;"), 100);
}

// §12.7: derived-only FIELD read on an is-narrowed variable.
BEZEL_TEST(codegen_isnarrow, derived_only_field_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal public virtual method Legs(): int => 0; end Animal; "
        "class Dog : Animal "
        "  public _tag: int; "
        "  public constructor() _tag := 77; end constructor; "
        "  public override method Legs(): int => 4; "
        "end Dog; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dog(); "
        "    var animal: Animal := d; "
        "    if animal is Dog then "
        "      return animal._tag; "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;"), 77);
}

// §12.7: three-level inheritance — narrow a C1-typed variable to C3 and call
// C3's own method (declared only on C3).
BEZEL_TEST(codegen_isnarrow, three_level_inheritance) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C1 public virtual method V(): int => 1; end C1; "
        "class C2 : C1 public override method V(): int => 2; end C2; "
        "class C3 : C2 "
        "  public override method V(): int => 3; "
        "  public method Special(): int => 99; "
        "end C3; "
        "class C "
        "  public static method Main(): int "
        "    using var obj := new C3(); "
        "    var b: C1 := obj; "
        "    if b is C3 then "
        "      return b.Special(); "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;"), 99);
}

// §12.7: interface-base narrowing — the variable's static type is the
// interface IAnimal; narrow to Dog and call Dog's own (non-interface) method.
BEZEL_TEST(codegen_isnarrow, interface_base_narrows) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IAnimal method Legs(): int; end IAnimal; "
        "class Dog : IAnimal "
        "  public _tag: int; "
        "  public constructor() _tag := 77; end constructor; "
        "  public method Legs(): int => 4; "
        "  public method Bark(): int => _tag + 23; "
        "end Dog; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dog(); "
        "    var animal: IAnimal := d; "
        "    if animal is Dog then "
        "      return animal.Bark(); "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;"), 100);
}

// §12.7: `is not T` narrows in the ELSE branch. The else block calls a
// derived-only method on the narrowed variable.
BEZEL_TEST(codegen_isnarrow, is_not_narrows_in_else) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal public virtual method Legs(): int => 0; end Animal; "
        "class Dog : Animal "
        "  public _tag: int; "
        "  public constructor() _tag := 77; end constructor; "
        "  public method Bark(): int => _tag + 23; "
        "end Dog; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dog(); "
        "    var animal: Animal := d; "
        "    if animal is not Dog then "
        "      return -1; "
        "    else "
        "      return animal.Bark(); "
        "    end; "
        "  end Main; "
        "end C;"), 100);
}

// §12.7 regression guard: a VIRTUAL method declared on the base, invoked
// after narrowing, still dispatches correctly through the base-valid `this`.
// (This path worked before §12.7; narrowing must not break it.) Dog overrides
// Legs to return 4, so virtual dispatch must yield 4 — not the base 0.
BEZEL_TEST(codegen_isnarrow, base_virtual_dispatch_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal public virtual method Legs(): int => 0; end Animal; "
        "class Dog : Animal "
        "  public override method Legs(): int => 4; "
        "end Dog; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dog(); "
        "    var animal: Animal := d; "
        "    if animal is Dog then "
        "      return animal.Legs(); "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;"), 4);
}
