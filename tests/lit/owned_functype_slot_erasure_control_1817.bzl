// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// CONTROL reject for issue #1817 sub-task E2 — §28.1 owned-marker erasure is
// gated STRICTLY on value/runtime-managed substitutions (spec §28.1/§27.1,
// v0.196). At a CLASS substitution the marker is RETAINED (its ordinary
// ownership-transfer meaning), so the substituted function type still carries
// `owned` and a markerless target is REJECTED by §27.1 exact-modifier
// assignability — exactly as before v0.196.
//
// This is the same program as owned_functype_slot_erasure_1817.bzl's
// parameter-slot arm, but instantiated at `T = Conn` (a class) instead of a
// value type: the E2 erasure must NOT fire, so `((owned Conn) -> ())?` does not
// convert to the markerless `((Conn) -> ())?`. If a future change over-erased
// (e.g. by ignoring the marker in typeRefEqual, or by dropping the value-kind
// gate), this reject would wrongly turn into an accept.
//
// The §27.1 diagnostic carries a precise column, matched by message text.
module M;
class Conn
  public constructor() end constructor;
end Conn;
class R<T>
  public constructor() end constructor;
  public method Id(): ((owned T) -> ())?
    return null;
  end Id;
end R;
class Program
  public static method Main(): int
    using var r := new R<Conn>();
    var g: ((Conn) -> ())? := r.Id();
    // CHECK: parameter 1 is `owned` but the target's is (none)
    // CHECK-SAME: (§27.1
    return 0;
  end Main;
end Program;
