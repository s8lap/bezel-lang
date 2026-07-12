// Ported from tests/ownership/using_var_return_762_test.cpp
//   (BEZEL_TEST using_var_return_762, plain_owned_local_return_accepted).
//
// §28.3: a plain (non-`using`) owned local is the transfer form the spec points
// to ("restructure to introduce the value as a non-`using var` owned local") —
// `return w` moves it out to the caller. ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Widget destructor end destructor; public static method Ok(): owned Widget var w := new Widget(); return w; end Ok; public static method Main(): int return 0; end Main; end Widget;

// NODIAG-NOT: error
// NODIAG-NOT: §
