// Ported from tests/ownership/closure_escape_outer_local_861_test.cpp
//   (BEZEL_TEST closure_escape_outer_local_861, same_scope_closure_accepted).
//
// §28.3: the closure is assigned to a SAME-scope local — it dies with `rr`, so
// it never outlives the capture. Accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public V: int; public constructor(v: int) V := v; end; destructor V := 0; end; end R; class C public static method Main(): int using var rr := new R(55); var f: () -> int := lambda() => rr.V; return f(); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
