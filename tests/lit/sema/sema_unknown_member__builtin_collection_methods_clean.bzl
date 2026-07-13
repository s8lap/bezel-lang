// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, builtin_collection_methods_clean).
//
// Builtin collection methods (no sema MemberSymbol) stay permissive (#420/#421).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(a: Array<int>): int a.Add(1); var n := a.Count; var f := a.First(); return n + f; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
