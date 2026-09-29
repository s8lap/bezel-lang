// narrow #4182 (stdlib §8.1, v0.298): REFUTATION. The issue claims field-access
// receivers (`h.sw.Start()`, `obj.Sw.Stop()` for a plain stored field) "keep
// the OLD, spurious-warning behaviour" — that they warn even though the call IS
// correctly dispatched, just like the pre-#4171 local-variable case. Measured at
// HEAD (2334d642): `h.sw.Start()/Stop()/Reset()` compiles with ZERO diagnostics.
// The issue description does not match the current behaviour — no spurious warning
// is produced for field-access receivers, only for local-variable receivers that
// the #4171 fix was scoped to deliberately. This test passes at HEAD as-is (no
// fix needed): `h.sw.Start()/Stop()/Reset()` are silently correctly handled.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --allow-empty %s

module M4182;

class HasSw
  public sw: Stopwatch;
  public constructor() sw := Stopwatch.StartNew(); end constructor;
end HasSw;

class C4182
  public static method Main(): int
    var h := new HasSw();
    // The issue's repro shape: plain field-access receiver.
    // Must be SILENT for Start/Stop/Reset — the call really IS dispatched.
    h.sw.Start();
    h.sw.Stop();
    h.sw.Reset();
    return 0;
  end Main;
end C4182;

// CHECK-NOT: cannot dispatch member 'Start'
// CHECK-NOT: cannot dispatch member 'Stop'
// CHECK-NOT: cannot dispatch member 'Reset'