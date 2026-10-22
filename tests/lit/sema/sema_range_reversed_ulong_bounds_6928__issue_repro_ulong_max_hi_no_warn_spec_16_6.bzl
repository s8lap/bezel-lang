// #6928 (§16.6, v0.340): `case u of 0..18446744073709551615UL` over a ulong
// scrutinee is a valid range that covers every ulong value. The upper bound is
// the ulong maximum, which folds to -1 as a long long (#6916's readback reads it
// back as unsigned over a ulong scrutinee). The bounds are not reversed, so the
// pattern must not warn.
//
// Non-vacuity: the first RUN line requires the program to compile (exit 0), so
// the silent second RUN line is checked against an accepted program.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(): int var u: ulong := 3; return (case u of 0..18446744073709551615UL => 1, _ => 0); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
