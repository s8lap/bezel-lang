// RUN: %bezelc -c -o %t.o %s
//
// §6.6, v0.313 / §5.1 / §17.14 (#5217): same rule as
// tests/lit/sema/char_compound_named_uint_const_rhs_accepted_5217.bzl, with
// a LOCAL `const L: uint := 3;`. A compile-time-classified local const
// (initializer is a constant expression by §17.14's inductive set, declared
// type is a primitive value type — §17.14's two conditions) is a §17.14
// constant expression (§21.1 v0.184, #1688); §5.1's constant narrowing
// therefore admits `c += L` (L's value 3 fits char's [0, 0x10FFFF] range).
// Before the fix the #5211 predicate `isLitDerived` was purely syntactic
// literal-derived and false-rejected the local-const reference.

module M; class C public static method Main(): int const L: uint := 3; var c: char := char(100); c += L; return int(c); end Main; end C;