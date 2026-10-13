// RUN: %not %bezelc -c %s 2>&1 | FileCheck --check-prefix=CHECK %s
//
// fix-forward #3575 (§35.4 / §35.3 / §28.8, v0.334): the METHOD spelling of the
// read. `Box.Get` returns the owned `Box._h` field by reading it, so its result
// is a BORROW, not a fresh producer — §35.3's "owned move-only value"
// classification belongs to a CONSTRUCTION, not to a read. An `owned` sink must
// therefore reject it under §28.8.
//
// This row is the control for its two siblings
// (`owning_choice_call_through_function_typed_field_3575_rejected.bzl` and
// `owning_choice_call_through_function_typed_local_3575_rejected.bzl`): the
// corrected arm tests positively for a case-construction symbol and no longer
// special-cases the method spelling, so this row is what shows that
// replacement kept the method case rejecting.

module M;
class Resource
  private _n: int;
  public constructor(n: int) _n := n; end constructor;
  public method N(): int => _n;
end Resource;
choice Bundle
  Ready(owned r: Resource),
  Empty;
end Bundle;
class Box
  public owned _h: Bundle;
  public constructor() _h := Bundle.Ready(new Resource(42)); end constructor;
  public method Get(): Bundle => _h;
  public method Take(owned h: Bundle) end;
end Box;
class C
  public static method Main(): int
    var b := new Box();
    var h := b.Get();
    b.Take(move h);
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow cannot be passed
// CHECK-SAME: (§28.8