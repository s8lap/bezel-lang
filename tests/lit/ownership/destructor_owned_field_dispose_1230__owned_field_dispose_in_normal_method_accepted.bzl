// Ported from tests/ownership/destructor_owned_field_dispose_1230_test.cpp
//   (BEZEL_TEST destructor_owned_field_dispose_1230, owned_field_dispose_in_normal_method_accepted).
//
// §28.5 (#1230 anti-over-reject): explicit `.Dispose()` of an owned field in a
// NORMAL (non-destructor) method is a legal explicit-dispose pattern (the field
// must be reassigned or moved-from before the method exits). The destructor-body
// restriction does NOT apply outside the destructor. ACCEPTED (no double-free
// because the destructor auto-cleanup only runs once).
// Note: the field IS restored here (_sub := new Inner()) to satisfy §28.8.1.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Inner public n: int := 0; destructor end destructor; end Inner; class Widget private owned _sub: Inner; public constructor() _sub := new Inner(); end constructor; public method Reset(): () using var old := move _sub; _sub := new Inner(); end Reset; destructor end destructor; public static method Main(): int return 0; end Main; end Widget;

// NODIAG-NOT: error
// NODIAG-NOT: §
