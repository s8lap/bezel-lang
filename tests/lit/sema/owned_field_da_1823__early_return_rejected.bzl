// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §7.9 (Appendix C.1), v0.196 — non-nullable field definite assignment at
// constructor normal exit. Regression for #1823 (early-`return` normal exit).
//
// An explicit `return` from a constructor is a normal-exit path: on `b=true`
// the constructor returns BEFORE `_box` is assigned, so the object escapes with
// a null `_box`. The check fires at the `return`, where `_box` is not yet
// definitely assigned — a later assignment cannot rescue a path that already
// returned. REJECTED.

module M;
class Box public value: int; public constructor() value := 7; end constructor; end Box;
sealed class W
  private owned _box: Box;
  public constructor(b: bool)
    if b then return; end;
    _box := new Box();
  end constructor;
  public method V(): int return _box.value; end V;
end W;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: constructor does not initialize non-nullable field `_box`
// CHECK-SAME: (§7.9
