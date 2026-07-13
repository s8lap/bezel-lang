// Ported from tests/sema/field_init_restrictions_test.cpp
//   (BEZEL_TEST sema_field_init, own_primary_ctor_param_in_derived_init_accepted_931).
//
// §22.8 / §22.10 (#478, #931): a derived class's field initializer may still
// reference its OWN primary-constructor parameter (available before field
// initializers run); the exception is scoped to the current type.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; public class Base public _b: int; public constructor() _b := 1; end constructor; destructor end destructor; end Base; public class Derived(v: int): Base public _twice: int := v * 2; end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
