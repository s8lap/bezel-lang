// RUN: %bezelc -c -o %t.o %s
//
// §6.6, v0.313 / §5.1 / §17.14 (#5217): a `char` compound target's right
// operand is typed as in `int op e` under §6.2; §5.1's constant narrowing
// admits a compile-time constant (§17.14) of any integer type whose value
// fits the target's range, across signedness. `public const K: uint := 5;`
// is a §17.14 compile-time constant (§22.12 class-level const, integer
// type, constant-expression initializer), so `c += K` is admitted with K's
// value 5 narrowing to char under §5.1. Before the fix the #5211 predicate
// `isLitDerived` was purely syntactic literal-derived and false-rejected
// `c += K` (an `IdentifierExpr` referencing a class const is not
// literal-derived, but is a §17.14 constant).

module M; class C public const K: uint := 5; public static method Main(): int var c: char := char(100); c += K; return int(c); end Main; end C;