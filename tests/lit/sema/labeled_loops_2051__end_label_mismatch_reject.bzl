// #2051 — §1.6 / §21.5 (v0.201): an identifier closer that does not match the
// loop's own label (`end other;` on a loop labeled `scan`) is a compile error.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method Main() scan: for var i in 0..2 do break scan; end other; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: end label 'other' does not match the loop label 'scan'; close with 'end scan;' or 'end;'
