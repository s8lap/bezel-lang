// #6928 (§16.6, v0.340): `5..18446744073709551615UL` is a valid ulong range
// (5 through the ulong maximum). The upper bound folds to -1 and the pre-fix
// compiler reads it as less than 5, so it warns spuriously; this row fails on the
// pre-fix compiler and passes with the fix.
//
// Non-vacuity: the first RUN line requires the program to compile (exit 0).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(): int var u: ulong := 3; return (case u of 5..18446744073709551615UL => 1, _ => 0); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
