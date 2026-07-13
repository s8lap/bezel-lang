// Ported from tests/sema/overload_signature_test.cpp
//   (BEZEL_TEST sema_overload, distinct_names_out_vs_out_owned_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class T end T; class C public method TryGet(out c: T?): bool c := null; return true; end TryGet; public method TryTake(out owned c: T?): bool c := null; return true; end TryTake; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
