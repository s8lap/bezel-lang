// Ported from tests/sema/out_owned_condition_892_test.cpp
//   (BEZEL_TEST out_owned_condition_892, if_borrow_form_accepted).
//
// §28.3 / #892 (BORROW-FORM GUARDRAIL): `Stack<int>.TryPop(out item)` —
// non-owning collection, value-typed element, no `owned` modifier — carries no
// discharge obligation. ACCEPT. Guards over-broad condition processing.
// v0.171 (#1336): the out param is `int?`, so the recipient is nullable.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int do const s: Stack<int> := [42]; var item: int? := null; if s.TryPop(out item) then end; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
