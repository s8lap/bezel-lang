// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/borrow_into_bare_field_1082_test.cpp
//   (BEZEL_TEST borrow_into_bare_field_1082, qualified_this_field_borrow_rejected).
//
// The qualified `this._cfg := cfg` was already rejected — still is.

module M; class Cfg public _v: int := 0; public constructor(v: int) _v := v; end constructor; end Cfg; class Holder public _cfg: Cfg?; public method Set() using var cfg := new Cfg(99); this._cfg := cfg; end Set; end Holder;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: borrow of `cfg` stored into a field would outlive its source — `cfg` is an owned scope-bound local disposed at scope end, and a field of the target object outlives it
// CHECK-SAME: (§28.3's borrowed-field store rule is holder-kind-conditioned: parameter- and receiver-sourced borrows are admissible with a recorded trust clause, while scope-bound locals and temporaries are rejected; store an owned value with `move`, or source the field's borrow from a receiver or parameter — the §28.5 back-reference idiom)
