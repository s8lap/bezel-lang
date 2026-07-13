// Ported from tests/sema/sealed_member_override_485_test.cpp
//   (BEZEL_TEST sema_sealed_member_485, override_of_virtual_still_ok).
//
// §12.1 regression guard — test 3: overriding a plain `virtual` member is OK.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A public virtual method Get(): int => 1; destructor end destructor; end A; class B : A public override method Get(): int => 2; destructor end destructor; end B;

// NODIAG-NOT: error
// NODIAG-NOT: §
