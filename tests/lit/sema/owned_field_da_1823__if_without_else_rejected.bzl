// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §7.9 (Appendix C.1), v0.196 — non-nullable field definite assignment at
// constructor normal exit. Regression for #1823 (the memory-safety hole).
//
// `_box` is assigned only inside `if b then … end` with no `else`: on the
// guard-false path it is NEVER assigned, so an object built with `b=false`
// escapes construction with a null `_box` and SIGSEGVs on first read. The
// any-path collector this replaces compiled it clean; the all-paths join
// REJECTS it.

module M;
class Box public value: int; public constructor() value := 7; end constructor; end Box;
sealed class W
  private owned _box: Box;
  public constructor(b: bool)
    if b then _box := new Box(); end;
  end constructor;
  public method V(): int return _box.value; end V;
end W;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: constructor does not initialize non-nullable field `_box`
// CHECK-SAME: (§7.9
