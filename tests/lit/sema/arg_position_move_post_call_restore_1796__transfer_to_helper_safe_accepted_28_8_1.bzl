// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Companion ACCEPT control for #1796 (spec §28.8.1, v0.193).
//
// §28.8.1: the "build then take then restore" discipline fixes the case
// from `...__transfer_to_helper_rejected_28_8_1.bzl` by moving the
// (throwable) call AFTER the restoration. The local is moved into
// `taken`, the field is restored, then the local is passed to the
// `owned` sink (no field state at risk on the call's exception edge).

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
  public method TransferToHelperSafe()
    var fresh := new Stream();
    var taken := move _resource;
    _resource := fresh;
    Sink.TakeOwnership(owned taken);
  end TransferToHelperSafe;
end Holder;

// NODIAG-NOT: error
// NODIAG-NOT: §
