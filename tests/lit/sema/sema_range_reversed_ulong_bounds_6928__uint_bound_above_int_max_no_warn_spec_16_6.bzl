// #6928 (§16.6, v0.340): a uint range whose bounds exceed int.MaxValue is valid
// and must not warn. The bounds fold non-negative, so the signed comparison is
// already exact; this row guards against a fix that treats large positive
// bounds as wrapped values.
//
// Control: this row passes on the pre-fix compiler too.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M(): int var u: uint := 3; return (case u of 3000000000U..4000000000U => 1, _ => 0); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
