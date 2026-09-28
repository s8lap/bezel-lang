// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, char_plus_int_accepted).
//
// `c += 1;` where `c: char` — fix-forward #5054 (§6.6, v0.307): §6.6's
// narrow-integer write-back names this pair explicitly — "For a `char`
// target the §5.2 code-point rules apply: `c += 1` is `c := char(c + 1)`" —
// so the compound-assignment FORM is accepted even though the raw BINARY
// `c + 1` (§3.5) is rejected (see gtest suite char_arithmetic_rejected_5054,
// tests/sema/char_arithmetic_rejected_5054_test.cpp, for that raw-binary
// control). Fix #5054's original commit (2c20822a) mistakenly rejected
// this compound form too, by routing
// it through the same §3.5 raw-binary gate unconditionally; the
// fix-forward scopes that gate off for exactly the (char target, integer
// rhs) compound-assignment / `++`/`--` pair.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var c: char := 'a'; c += 1; return int(c); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
