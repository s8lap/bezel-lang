// Ported from tests/sema/ctor_chain_test.cpp
//   (BEZEL_TEST sema_ctor_chain, no_chain_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _x: int := 0; public constructor() this._x := 1; end constructor; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
