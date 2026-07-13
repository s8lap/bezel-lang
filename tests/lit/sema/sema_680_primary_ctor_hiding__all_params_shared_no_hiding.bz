// Ported from tests/sema/hiding_680_primary_ctor_param_test.cpp
//   (BEZEL_TEST sema_680_primary_ctor_hiding, all_params_shared_no_hiding).
//
// §11.1 / #680: all params shared in both base and derived — no hiding error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base(a: int, b: int) private _a: int := a; private _b: int := b; end Base; class Derived(a: int, b: int) : Base(a, b) private _extra: int := a; end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
