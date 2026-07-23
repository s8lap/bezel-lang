// #2051 — §21.5 (v0.201): a labeled `break`/`continue` whose label names no
// lexically-enclosing loop is a compile error naming the labels in scope.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method Main() scan: for var i in 0..2 do break other; end scan; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unknown loop label 'other' in `break` (§21.5); labels in scope: 'scan'
