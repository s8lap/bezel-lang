// #2051 — §21.5 (v0.201): only loop statements take a loop label. A label prefix
// on a non-loop statement (here an `if`) is a compile error.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method Main() lbl: if true then Console.WriteLine("x"); end; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a loop label may prefix only a loop statement — `for`, `while`, or `await for` (§21.5)
