// Ported from tests/sema/ctor_chain_test.cpp
//   (BEZEL_TEST sema_ctor_chain, base_call_first_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class B public constructor() end constructor; end B; class D : B public _x: int := 0; public constructor() base(); this._x := 1; end constructor; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
