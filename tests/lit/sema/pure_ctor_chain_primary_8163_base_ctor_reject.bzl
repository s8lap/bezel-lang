// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §22.15 "Pure constructors" (v0.355): "A pure constructor's `base(...)`/
// `this(...)` chain target must itself be pure." The `this(...)` chain
// reaches E's primary, whose inheritance clause invokes ImpureBase's
// non-pure body constructor (#8163).
//
// CHECK-NOT: internal compiler error

module M;
class ImpureBase : Exception
  public constructor(m: string) base(m);
    Console.WriteLine("impure base ctor");
  end constructor;
end ImpureBase;
class E(m: string): ImpureBase(m)
  public pure constructor() this("d"); end constructor;
end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: constructor chain call is not admitted in a pure body
// CHECK-SAME: base(...) target is not pure
// CHECK-SAME: (§22.15)
