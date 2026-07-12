// Ported from tests/sema/field_init_restrictions_test.cpp
//   (BEZEL_TEST sema_field_init, spec_0025_enum_static_readonly_references_own_constants_ok).
//
// §22.8 / §10 fix #656: enum constants are implicitly static compile-time
// values; a static readonly field initializer on an enum may freely reference
// the enum's declared constants. Spec §10 / §22.13: "enum constants are
// compile-time values and are available before any static readonly initializer
// runs." spec_0025 (§10.4 Methods, properties, static members) verifies this.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Priority Low, Medium, High; public static readonly All: int := Low; end Priority;

// NODIAG-NOT: error
// NODIAG-NOT: §
