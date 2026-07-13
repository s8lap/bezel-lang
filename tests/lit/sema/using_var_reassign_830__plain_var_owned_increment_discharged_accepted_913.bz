// Ported from tests/sema/using_var_reassign_830_test.cpp
//   (BEZEL_TEST using_var_reassign_830, plain_var_owned_increment_discharged_accepted_913).
//
// fix #913: the same owned `c++` is ACCEPTED when the local IS discharged
// (`.Dispose()`), mirroring a discharged direct reassignment — the §28.3 reject
// is specifically the UNdischarged case.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Counter public Value: int := 0; public constructor(v: int) Value := v; end constructor; destructor Value := 0; end destructor; public static method operator ++(c: Counter): owned Counter => new Counter(c.Value + 1); end Counter; class C public static method Main(): int var c := new Counter(0); c++; c.Dispose(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
