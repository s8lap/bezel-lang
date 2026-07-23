// #2051 — §1.6 / §21.5 (v0.201): an identifier closer (`end scan;`) on an
// UNLABELED loop is a compile error — the loop has no name to repeat.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method Main() for var i in 0..2 do Console.WriteLine("x"); end scan; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: end label 'scan' does not match: this 'for' loop is unlabeled — close with 'end for;' or 'end;'
