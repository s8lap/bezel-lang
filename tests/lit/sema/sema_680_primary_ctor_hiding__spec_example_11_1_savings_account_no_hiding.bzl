// Ported from tests/sema/hiding_680_primary_ctor_param_test.cpp
//   (BEZEL_TEST sema_680_primary_ctor_hiding, spec_example_11_1_savings_account_no_hiding).
//
// §11.1 spec example (verbatim shape): base and derived class both have
// primary-ctor parameters named `name` and `initialBalance`. The derived
// class's `SavingsAccount(name, initialBalance, rate)` must NOT produce a
// 'hides an inherited member' error on `name` or `initialBalance`.
// Pre-fix: false positive on both params. Post-fix: compiles clean.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class BankAccount(name: string, initialBalance: int) public readonly property Name: string := name; private _balance: int := initialBalance; end BankAccount; class SavingsAccount(name: string, initialBalance: int, rate: int) : BankAccount(name, initialBalance) private _rate: int := rate; end SavingsAccount;

// NODIAG-NOT: error
// NODIAG-NOT: §
