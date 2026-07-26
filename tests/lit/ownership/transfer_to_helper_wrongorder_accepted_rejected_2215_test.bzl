// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression for issue #2215 / spec §28.8.1 (v0.193).
//
// §28.8.1 "Exception paths are not exempt": an argument-position move whose
// restoration follows the call leaves the field moved-from on the call's
// exception edge (the restore never runs on that exit). Method calls are
// throwable, so the §28.8.1 field-restoration diagnostic (#867) must fire on
// `M(move x.f); x.f := ...` even when the call site is a synchronous
// `ExpressionStmt`.
//
// The architectural fix lands in flow/CFG.cpp: every `ExpressionStmt` whose
// expression contains a CallExpr / NullCondCallExpr / NewExpr gets split
// into a fresh Linear block carrying the call, with an exception edge from
// that block to the active try handler (or the function exit). The dataflow's
// pre-call state therefore propagates to the exception target where
// dischargeOnExceptionEdge auto-disposes owned locals, but the field-move
// facts pass through untouched — so the §28.8.1 OR-merge at endFlow.fields
// sees a moved-from field on at least one exit-reaching path.
//
// This pin uses a NON-`this` receiver (`Holder`) for symmetry with the
// canonical `TransferToHelper` example from spec §28.8.1; the diagnostic
// gates on the moved-from fact, not the receiver.

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
    Sink.TakeOwnership(move _resource);
    _resource := fresh;
  end TransferToHelper;
end Holder;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: field `_resource` left in moved-from state at method exit
// CHECK-SAME: (§28.8.1
