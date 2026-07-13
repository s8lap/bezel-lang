// Ported from tests/sema/duplicate_top_level_type_747_test.cpp
//   (BEZEL_TEST duplicate_top_level_type_747, distinct_names_ok).
//
// §11.3 regression guard: distinct top-level type names are accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A public static V: int := 1; end A; class B public static V: int := 2; end B; record C(X: int); enum E Red, Green; end E; interface I method M(): int; end I; class P public static method Main(): int return A.V + B.V; end Main; end P;

// NODIAG-NOT: error
// NODIAG-NOT: §
