// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §27.3 / §27.1.1 / §30.6 / §21.7, v0.217 (impl #2101) — the return-type
// ascription's REJECT surface:
//   1. Against a target type the ascription must AGREE under §27.1.1's
//      covariant return rule — it never bridges a conversion (here: `: int`
//      vs a `long` target return slot; primitives are invariant).
//   2. A unit ascription declares a PROCEDURE lambda, which does not convert
//      to a value-returning target ("nor the reverse", §27.3).
//   3. An `async` lambda's ascription is its declared task-family return
//      (§30.6/§30.1); a non-task ascription is rejected.
//   4. A value ascription imposes the method totality rule: a block body must
//      return on every path (§27.3/§21.7).
module M;
class C
  public static method A(): int
    var f: (int) -> long := lambda(x: int): int => x;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for `f` is a lambda whose return-type ascription does not agree with the target function type's return slot
    // CHECK-SAME: never bridges a conversion
    // CHECK-SAME: (§27.3 v0.217, §27.1.1)
    return 0;
  end A;

  public static method B(): int
    var g: (int) -> int := lambda(x: int): () => x;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for `g` is a lambda whose return-type ascription does not agree with the target function type's return slot
    return 0;
  end B;

  public static method D(): int
    var h := async lambda(): int => 1;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: an `async` lambda's return-type ascription must be `Task`, `Task<T>`, or `OwningTask<T>`
    // CHECK-SAME: (§30.6, §30.1; §27.3 v0.217)
    return 0;
  end D;

  public static method E(): int
    var k := lambda(x: int): int => do
      if x > 0 then return 1; end;
    end;
    // CHECK: lambda has a return-type ascription but does not return a value on every path
    // CHECK-SAME: (§27.3, §21.7)
    return 0;
  end E;
end C;
