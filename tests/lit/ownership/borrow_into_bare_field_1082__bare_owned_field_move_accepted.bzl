// Ported from tests/ownership/borrow_into_bare_field_1082_test.cpp
//   (BEZEL_TEST borrow_into_bare_field_1082, bare_owned_field_move_accepted).
//
// A bare OWNED field receiving a `move` of an owned local is a legal transfer,
// NOT a borrow store — it must still be accepted (the owned-field move-on-read
// skip recognises a bare-identifier owned field).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cfg public _v: int := 0; public constructor(v: int) _v := v; end constructor; end Cfg; class Holder public owned _c: Cfg?; public method Set() var c := new Cfg(5); _c := c; end Set; end Holder;

// NODIAG-NOT: error
// NODIAG-NOT: §
