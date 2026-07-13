// Ported from tests/sema/private_visibility_test.cpp
//   (BEZEL_TEST sema_priv, private_member_of_private_type_accepted_956).
//
// ACCEPT: a PRIVATE member may use a private nested type (unconstrained), and a
// public field of a public (builtin) type is fine.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; public class Outer private class Token destructor end destructor; end Token; private _t: Token?; public _x: int; private method Make(): owned Token => new Token(); destructor end destructor; public static method Main(): int => 0; end Outer;

// NODIAG-NOT: error
// NODIAG-NOT: §
