// Ported from tests/sema/override_signature_test.cpp
//   (BEZEL_TEST sema_override, matching_override_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base public virtual method F(a: int): int => a; end Base; class D : Base public override method F(a: int): int => a; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
