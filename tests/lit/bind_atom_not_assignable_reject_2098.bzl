// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §17.15, v0.216 (impl #2098) — "It is binding, not
// assignment-in-expression … Like a runtime `const` (§21.1), a binding is
// NOT an assignable location: assignment, compound assignment, `++`/`--`,
// and binding to a `ref` or `out` parameter are compile errors naming this
// rule (assignment forms are statements and cannot be spelled inside the
// atom at all; the location rule additionally covers a `do`-bodied lambda
// within the atom that captures the binding)."
module M;
class C
  public static method Run(f: () -> ()): int
    f();
    return 0;
  end Run;
  public static method Bump(ref x: int): int
    x := x + 1;
    return x;
  end Bump;

  // (1) Assignment through a `do`-bodied lambda capturing the binding — the
  // exact hole §17.15 names.
  public static method A(): int
    return (var n := 1; C.Run(lambda() => do n := 5; end));
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot assign to binding-atom binding `n`
    // CHECK-SAME: not an assignable location (§17.15, v0.216)
  end A;

  // (2) `++` through a `do`-bodied lambda.
  public static method B(): int
    return (var n := 1; C.Run(lambda() => do n++; end));
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot increment/decrement binding-atom binding `n`
    // CHECK-SAME: (§17.15, v0.216)
  end B;

  // (3) Binding to a `ref` parameter.
  public static method D(): int
    return (var n := 1; C.Bump(ref n));
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: a `ref` argument may not be a binding-atom binding `n`
    // CHECK-SAME: (§17.15, v0.216)
  end D;
end C;
