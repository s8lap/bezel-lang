// §7.4 (v0.214) / §22.16 / #2116 (pre-gate review): contract clauses evaluate
// at the member's BOUNDARIES — `requires` at entry, before any body guard has
// run — so the body's FINAL flow state must not admit a nullable
// readonly-field access in a clause: the lowered requires prologue would
// fault at entry on `new C(null)`. (Probe-confirmed SEGV.) The clause's
// unguarded `_name.Length` is REJECTED.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Len(): int requires(_name.Length >= 0) if _name = null then return 0; end; return _name.Length; end Len; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot narrow `T?` accessed through a field
// CHECK-SAME: (§7.4)
