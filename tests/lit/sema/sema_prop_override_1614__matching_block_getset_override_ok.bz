// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, matching_block_getset_override_ok).
//
// §12.1 / #1614: a matching get+set block-body override (base get+set,
// override get+set) compiles.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base private _x: int := 0; public virtual property X: int get => _x; set(v) => _x := v; end; end Base; class D : Base private _y: int := 0; public override property X: int get => _y; set(v) => _y := v; end; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
