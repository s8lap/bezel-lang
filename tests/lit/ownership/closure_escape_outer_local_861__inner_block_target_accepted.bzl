// Ported from tests/ownership/closure_escape_outer_local_861_test.cpp
//   (BEZEL_TEST closure_escape_outer_local_861, inner_block_target_accepted).
//
// §28.3: the closure target `g` is declared in an INNER block, strictly deeper
// than the captured `rr`. `g` dies before `rr`, so it does not outlive the
// capture — not an escape, accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public V: int; public constructor(v: int) V := v; end; destructor V := 0; end; end R; class C public static method Main(): int using var rr := new R(55); do var g: () -> int := () => rr.V; var n := g(); end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
