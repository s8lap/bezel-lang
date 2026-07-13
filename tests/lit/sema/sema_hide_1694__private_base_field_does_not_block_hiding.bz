// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide_1694, private_base_field_does_not_block_hiding).
//
// §12.2 (v0.184, issue #1694): "The blocked-name set." A `private` base
// member never blocks a same-named derived member — "two same-named
// `private` fields in base and derived are distinct members and legal."
// Prior to the fix, the hiding check collected every same-name base member
// regardless of visibility, so this was wrongly rejected. This is the
// fail-without/pass-with regression pin for #1694.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base private _x: int; end Base; class Derived : Base private _x: int; end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
