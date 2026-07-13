// Ported from tests/sema/private_visibility_test.cpp
//   (BEZEL_TEST sema_priv, nested_class_accesses_enclosing_private_954).
//
// ACCEPT: a nested class reads the enclosing class's private field.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; public class Outer private _value: int; public constructor(v: int) _value := v; end constructor; destructor end destructor; public class Accessor public static method GetValue(o: Outer): int => o._value; destructor end destructor; end Accessor; public static method Main(): int return 0; end Main; end Outer;

// NODIAG-NOT: error
// NODIAG-NOT: §
