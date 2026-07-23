// §7.4 (v0.214) / #2118 / #2116: a `while` body may execute ZERO times, so a
// readonly-field fact established INSIDE the body (the early-return guard)
// must not survive past the loop — on the zero-iteration path the guard never
// ran. The after-loop `_name.Length` is REJECTED. (The field-fact analogue of
// #2118's local-narrowing loop join.)
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Len(n: int): int var i := 0; while i < n do if _name = null then return 0; end; i := i + 1; end; return _name.Length; end Len; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on nullable field `_name` requires a local copy first
// CHECK-SAME: (§7.4)
