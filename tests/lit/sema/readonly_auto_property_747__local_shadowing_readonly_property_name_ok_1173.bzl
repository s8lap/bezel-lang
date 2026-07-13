// Ported from tests/sema/readonly_auto_property_747_test.cpp
//   (BEZEL_TEST readonly_auto_property_747, local_shadowing_readonly_property_name_ok_1173).
//
// §23.1 (#1173): a LOCAL variable that shadows a readonly property's name must
// NOT be falsely flagged — it resolves to a LocalSymbol, not the PropertySymbol.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A public readonly property X: int := 5; public constructor() end constructor; public method Calc(): int var X: int := 3; X := 8; return X; end Calc; public static method Main(): int return 0; end Main; end A;

// NODIAG-NOT: error
// NODIAG-NOT: §
