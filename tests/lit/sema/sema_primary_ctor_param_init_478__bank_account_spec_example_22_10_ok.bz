// Ported from tests/sema/primary_ctor_param_init_478_test.cpp
//   (BEZEL_TEST sema_primary_ctor_param_init_478, bank_account_spec_example_22_10_ok).
//
// §22.10 spec example: BankAccount with `name` and `initialBalance`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class BankAccount(name: string, initialBalance: int) public readonly property Name: string := name; public _balance: int := initialBalance; end BankAccount;

// NODIAG-NOT: error
// NODIAG-NOT: §
