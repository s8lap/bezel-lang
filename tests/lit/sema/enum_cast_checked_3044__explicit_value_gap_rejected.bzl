// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3044 (§5.4 / §10.2, v0.242): with EXPLICIT values (§10.2) the declared
// set is sparse, so a constant that falls in a gap is equally a compile error —
// the rule is over the declared VALUES, not over a range. 300 lies between
// `Created := 201` and `NotFound := 404`, well inside the underlying `int`
// range, so the former #859 range check said nothing about it; §5.4's checked
// rule subsumes that check and rejects it.

module M; enum HttpStatus Ok := 200, Created := 201, NotFound := 404; end HttpStatus; class C public static method Main(): int var s: HttpStatus := HttpStatus(300); return int(s); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum cast `HttpStatus(300)` — 300 is not the underlying value of any declared constant of `HttpStatus`
// CHECK-SAME: (§5.4)
