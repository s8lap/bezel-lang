// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §7.9 (Appendix C.1), v0.196 — non-nullable field definite assignment at
// constructor normal exit. Regression for #1823 (loop-body over-accept).
//
// `_box` is assigned only inside a `while` body, which may run zero times, so
// it is not definitely assigned on the loop's normal-exit path. REJECTED.

module M;
class Box public value: int; public constructor() value := 7; end constructor; end Box;
sealed class W
  private owned _box: Box;
  public constructor(b: bool)
    while b do _box := new Box(); end;
  end constructor;
  public method V(): int return _box.value; end V;
end W;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: constructor does not initialize non-nullable field `_box`
// CHECK-SAME: (§7.9
