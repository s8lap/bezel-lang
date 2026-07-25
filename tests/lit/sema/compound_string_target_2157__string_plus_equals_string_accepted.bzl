// #2157 positive control (§4.6/§6.6, spec v0.226): `s += "cd"` — the one
// compound form defined on a string target (concatenation, §4.6), with a
// `string` RHS. Must stay admitted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var s: string := "ab"; s += "cd"; return s.Length; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
