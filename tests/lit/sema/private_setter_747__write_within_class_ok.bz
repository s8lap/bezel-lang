// Ported from tests/sema/private_setter_747_test.cpp
//   (BEZEL_TEST private_setter_747, write_within_class_ok).
//
// §13.6: writing the `private set` from WITHIN the declaring class is allowed.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Counter public _v: int := 0; public property Value: int get => _v; private set => do _v := value; end; end; public method Bump() this.Value := _v + 1; end Bump; end Counter; class Program public static method Main(): int return 0; end Main; end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
