// #2051 — §21.5 (v0.201): a loop label that repeats a label already in scope
// (an enclosing labeled loop of the same callable) is a compile error at the
// inner label — labels do not shadow.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method Main() scan: for var i in 0..2 do scan: for var j in 0..2 do break scan; end scan; end scan; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: loop label 'scan' duplicates the label of an enclosing loop; labels do not shadow (§21.5)
