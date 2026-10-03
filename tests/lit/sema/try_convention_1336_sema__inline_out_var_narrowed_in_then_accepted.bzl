// Issue #5312 / stdlib §1 (v0.313) + spec §22.4: `Stack<int>.TryPop` is a
// shape-(a) member, so `out var x` is narrowed non-null in the controlled
// `then` region and `return x` is ACCEPTED without `!`. This is the program
// the (c) reject test used to pin as rejected before #5312.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int do const s: Stack<int> := [1]; if s.TryPop(out var x) then return x; end; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
