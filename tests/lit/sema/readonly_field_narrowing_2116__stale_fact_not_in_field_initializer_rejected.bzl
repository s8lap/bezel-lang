// §7.4 (v0.214) / #2116: readonly-field facts are METHOD-BODY-LOCAL. A method
// body that ENDS with a live fact (the early-return guard shape) must not
// bleed that fact into a later-visited FIELD INITIALIZER — an initializer is
// not a body (it never passes the body-entry state reset) and runs in the
// §22.8 window where the guarded field may still be unassigned. The
// initializer's `Counter._label.Length` is REJECTED. (Pins the body-exit
// clear; found by the #2116 self-pass.)
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Counter public static readonly _label: string? := "hi"; public static method M(): int if _label = null then return 0; end; return _label.Length; end M; public static _n: int := Counter._label.Length; end Counter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)
