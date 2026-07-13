// Ported from tests/ownership/borrow_into_bare_field_1082_test.cpp
//   (BEZEL_TEST borrow_into_bare_field_1082, bare_field_borrow_of_param_accepted).
//
// Storing a borrow of a PARAMETER (which outlives the method) into a bare field
// is legal — the param's referent is the caller's, not a scope-bound local.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cfg public _v: int := 0; public constructor(v: int) _v := v; end constructor; end Cfg; class Holder public _cfg: Cfg?; public method Set(c: Cfg) _cfg := c; end Set; end Holder;

// NODIAG-NOT: error
// NODIAG-NOT: §
