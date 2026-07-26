// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §27.3, v0.226 (impl #2107) — the divergent-return error for a
// value-returning block-bodied lambda inferred WITHOUT a target type: when no
// target type, return-type ascription, or ascribed variable type is present,
// the body's value `return` expressions must all have the SAME type; the
// compiler does NOT unify them to a common supertype (an inference that
// silently computed `Animal` from `return dog;` and `return cat;` would make
// the lambda's type sensitive to distant hierarchy edits). Without #2107 the
// block-body inference took the FIRST return's type (accepts-invalid); with it
// the lambda is rejected and the message names the three v0.217 remedies.
module M;
class Dog end Dog;
class Cat end Cat;
class C
  // Primitive divergence: `return 1;` (int) vs `return "x";` (string).
  public static method Prim(): int
    var f := lambda() => do
      if true then return 1; end;
      return "x";
    end;
    // CHECK: :[[@LINE-4]]:{{[0-9]+}}: a block-bodied lambda with no target type has divergent `return` types
    // CHECK-SAME: give it a target type, a return-type ascription
    // CHECK-SAME: or an ascribed variable type
    // CHECK-SAME: (§27.3)
    return 0;
  end Prim;

  // Reference divergence: `return d;` (Dog) vs `return c;` (Cat). Without
  // #2107 this silently inferred `(Dog, Cat) -> Dog` (first-return-wins).
  public static method Ref(): int
    var g := lambda(d: Dog, c: Cat) => do
      if true then return d; end;
      return c;
    end;
    // CHECK: :[[@LINE-4]]:{{[0-9]+}}: a block-bodied lambda with no target type has divergent `return` types
    return 0;
  end Ref;
end C;
