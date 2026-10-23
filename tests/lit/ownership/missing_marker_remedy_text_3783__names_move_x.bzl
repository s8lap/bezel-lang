// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3783 (§22.3, v0.286): the §22.3 missing-marker diagnostic must name
// the v0.286 mandated spelling `M(move x)`, not the legacy `M(owned x)` that
// the compiler was emitting on this error path. The two spellings were
// interchangeable at v0.169 (#1348); from v0.286 onward the transfer marker
// at a call site is the identifier-operand `move` expression of §28.8.1 —
// `M(owned x)` reads as an argument modifier (A.7 `ArgModifiers`) and is not
// the fix the diagnostic should point at.
//
// §22.3: "Omitting the marker on a named owned binding is a compile error
// naming the fix ('this call consumes `x`; write `M(move x)`')".
//
// Spec-cited regression: a bare named owned binding passed into an `owned`
// parameter must emit the diagnostic naming the `M(move x)` spelling, in the
// exact form above. The `write \`M(move x)\`` fragment is the load-bearing
// part — a diagnostic that names `M(owned x)` instead is the very defect
// this lit row pins. The CHECK-NOT guards the negative half: the OLD
// spelling must not appear anywhere in the diagnostic output.

module M;
class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;
class Sink
  public method Take(owned x: Res)
    using var h := move x;
  end Take;
end Sink;
class C public method M(s: Sink)
  var x := new Res(7);
  s.Take(x);
end M;
end C;

// #7065: the same needle, written ABOVE the positive match. A negative
// directive scans only the region between the surrounding matches, so the
// copy below the match alone let this wording through when it came FIRST.
// CHECK-NOT: write `M(owned x)`
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this call consumes `x`; write `M(move x)` — passing the named owned binding `x` to an `owned` parameter transfers ownership and leaves it moved-from, which the call site must announce (§22.3
// CHECK-NOT: write `M(owned x)`