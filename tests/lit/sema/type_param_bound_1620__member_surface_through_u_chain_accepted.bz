// Ported from tests/sema/type_param_bound_1620_test.cpp
//   (BEZEL_TEST type_param_bound_1620, member_surface_through_u_chain_accepted).
//
// §14.2: `x.Speak()` on a `T`-typed value resolves through `where T: U` to
// `U`'s `Animal` constraint surface — accepted, no unconstrained-param error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Animal public method Speak(): string return "..."; end Speak; end Animal; class Dog: Animal end Dog; static class Utils public static method Describe<T, U>(x: T): string where T: U where U: Animal return x.Speak(); end Describe; end Utils; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
