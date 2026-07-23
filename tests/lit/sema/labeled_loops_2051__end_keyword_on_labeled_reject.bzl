// #2051 — §1.6 / §21.5 (v0.201): a labeled loop is a NAMED construct, so the
// keyword closer `end for;` no longer applies — it must close with `end scan;`
// or bare `end;`. `end for;` on a labeled loop is a compile error.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method Main() scan: for var i in 0..2 do break scan; end for; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this loop is named 'scan'; close with 'end scan;' or 'end;', not 'end for;' (§1.6/§21.5)
