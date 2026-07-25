// #2155 positive control (§6.2/§6.6, spec v0.226): `int += int` — same type
// on both sides, R = T directly, no §6.2 promotion and no narrow-integer
// write-back. Must stay admitted (the §6.2 reject fires only for the
// no-conversion-either-way pair).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var i: int := 1; i += 2; return i; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
