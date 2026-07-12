// Ported from tests/sema/private_visibility_test.cpp
//   (BEZEL_TEST sema_priv, enclosing_class_accesses_nested_private_954).
//
// ACCEPT: the enclosing class reads a nested type's private field.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; public class Outer public class Inner private _secret: int; public constructor(v: int) _secret := v; end constructor; destructor end destructor; end Inner; destructor end destructor; public method ExtractSecret(i: Inner): int => i._secret; public static method Main(): int return 0; end Main; end Outer;

// NODIAG-NOT: error
// NODIAG-NOT: §
