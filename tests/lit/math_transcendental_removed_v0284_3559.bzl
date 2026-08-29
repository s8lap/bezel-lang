// fix #3559 (stdlib §2.2 `Math`, v0.285) — the transcendental block was REMOVED
// from the documents at v0.284 (proposal 0121 R3), together with the
// never-documented `Hypot`, `Cbrt` and the duplicate `PI` spelling (the G1
// ruling, issue 3411). The compiler accepted all of them until this fix, so
// each was an impl-ahead-of-spec delta.
//
// WHY THE WORDING IS THE TEST. `Math.Sin` was a real member one revision ago.
// "'Math' has no static method or property 'Sin'" — the diagnostic every OTHER
// unknown `Math` member gets — would send a reader hunting for a typo that is
// not there. The message has to say the member was removed, say at which
// revision, and say what SURVIVED, so a numeric program can be repaired rather
// than merely refused.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M3559Math;

class C
  public static method Main(): int
    var s := Math.Sin(1.0);
    var e := Math.E;
    return int(s + e);
  end Main;
end C;

// A withdrawn method, located at the call, never 0:0.
// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: 'Math.Sin' was REMOVED from the standard library at v0.284 (stdlib §2.2)
// CHECK-SAME: it is gone, not misspelled (issue 3559)
// CHECK-SAME: `Sqrt`, `Pow`, `DivRem`, `Fmod`, `IEEERemainder`, the rounding family and the constants `Pi` and `Tau` remain

// The withdrawn CONSTANT reaches the same diagnostic — `E` is a member-access
// spelling, not a call, and it has its own codegen path in
// `LlvmBackend_Exprs.cpp` that the sema rejection now shadows.
// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: 'Math.E' was REMOVED from the standard library at v0.284 (stdlib §2.2)
