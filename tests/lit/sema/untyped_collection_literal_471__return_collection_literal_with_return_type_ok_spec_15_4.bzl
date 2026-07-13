// Ported from tests/sema/untyped_collection_literal_test.cpp
//   (BEZEL_TEST untyped_collection_literal_471, return_collection_literal_with_return_type_ok_spec_15_4).
//
// §15.4: `return [1, 2, 3]` when the method's return type provides a target
// is OK. Spec says: "`return [1, 2, 3]` is admitted when the enclosing
// method's return type determines the target type."
// We verify this doesn't create a false positive (no diagnostic).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(): [3]int return [1, 2, 3]; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
