// Ported from tests/sema/send_600_spawn_callsite_test.cpp
//   (BEZEL_TEST send_600_spawn_callsite, non_capturing_lambda_entry_accepted_30_14).
//
// §30.14: A non-capturing lambda entry → accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Main public static async method Go(): Task using var t := Domain.Spawn(lambda() => ()); await t; end Go; end Main;

// NODIAG-NOT: error
// NODIAG-NOT: §
