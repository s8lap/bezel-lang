// Ported from tests/ownership/closure_escape_outer_local_861_test.cpp
//   (BEZEL_TEST closure_escape_outer_local_861, no_owned_capture_accepted).
//
// §28.3: a closure assigned to an enclosing local but capturing NO owned local
// (only a value-typed `int`) is not constrained — value captures carry no
// scope-bound disposal obligation. Accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var f: () -> int := lambda() => -1; do var k := 7; f := lambda() => k; end; return f(); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
