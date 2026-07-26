// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression for issue #1796 (spec §28.8.1, v0.193).
//
// §28.8.1 "Exception paths are not exempt": "an argument-position move whose
// restoration follows the call is rejected on the call's exception edge" —
// method calls are throwable, so `M(move x.f); x.f := ...` leaves the field
// moved-from on the M-throws exit (the restore never runs on that edge),
// which the field-restoration check (#867) catches.
//
// §28.8.1 (#867, path-sensitive): "the OR-over-every-path-reaching-exit
// merge of the per-block moved-from facts ... so a field restored on only
// SOME paths is reported here exactly because the unrestored path leaves
// it moved-from at the merged exit state".

module M;
class Stream
  public constructor() end constructor;
  destructor end destructor;
end Stream;
class Sink
  public static method TakeOwnership(owned s: Stream) s.Dispose(); end TakeOwnership;
end Sink;
class Holder
  private owned _resource: Stream;
  public constructor() _resource := new Stream(); end constructor;
  public method TransferToHelper()
    var fresh := new Stream();
    Sink.TakeOwnership(move _resource);   // arg move; call may throw
    _resource := fresh;                    // restore on normal edge only
  end TransferToHelper;
end Holder;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: field `_resource` left in moved-from state at method exit
// CHECK-SAME: (§28.8.1
