// Ported from tests/sema/private_setter_747_test.cpp
//   (BEZEL_TEST private_setter_747, public_setter_ok).
//
// §13.6: a public setter is writable from anywhere — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Counter public _v: int := 0; public property Value: int get => _v; set => do _v := value; end; end; end Counter; class Program public static method Main(): int using var c := new Counter(); c.Value := 42; return c.Value; end Main; end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
