// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, same_type_bitwise_accepted).
//
// `checksum ^= b;` where both are `byte` — R = T directly, no write-back.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var checksum: byte := 0; var b: byte := 254; checksum ^= b; return int(checksum); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
