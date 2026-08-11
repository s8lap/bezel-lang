// fix #3044 (§5.4, v0.242) OVER-REJECTION GUARD: a constant cast naming a
// DECLARED value stays accepted — "it succeeds exactly when `n` equals the
// underlying value of some declared constant of `E`, and yields that value".
// `Priority` declares 0, 1, 2, so `Priority(2)` is `High`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Priority Low, Medium, High; end Priority; class C public static method Main(): int var p: Priority := Priority(2); return int(p); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
