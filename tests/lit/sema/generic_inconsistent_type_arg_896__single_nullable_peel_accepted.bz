// Ported from tests/sema/generic_inconsistent_type_arg_896_test.cpp
//   (BEZEL_TEST generic_inconsistent_type_arg_896, single_nullable_peel_accepted).
//
// §14.1 / #896 / #850: a single-param nullable peel Identity<T>(v:T?):T
// with one int? arg — T=int, consistent. ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class R public static method Identity<T>(v: T?): T where T: value return v ?? default(T); end Identity; public static method Main(): int var x: int? := 5; return Identity(x); end Main; end R;

// NODIAG-NOT: error
// NODIAG-NOT: §
