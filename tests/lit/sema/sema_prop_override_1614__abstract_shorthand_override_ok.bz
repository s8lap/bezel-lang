// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, abstract_shorthand_override_ok).
//
// §12.5 / #1614: the abstract shorthand `abstract property Name: T;` declares
// a single get-only abstract getter; overriding it with an expression-bodied
// get-only property is the canonical idiom and must compile.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; abstract class Base public abstract property Name: string; end Base; class D : Base public override property Name: string => "d"; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
