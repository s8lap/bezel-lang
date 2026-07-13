// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, inherited_member_on_class_clean).
//
// §12: an inherited member (declared on a base class) resolves cleanly.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base public method Tick(): int => 1; end Base; class Derived : Base end Derived; class C public method M(d: Derived): int => d.Tick(); end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
