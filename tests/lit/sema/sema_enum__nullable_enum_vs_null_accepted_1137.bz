// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, nullable_enum_vs_null_accepted_1137).
//
// A nullable enum compared with `null` stays accepted (§8.4 null-safe equality).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Color Red, Green; end Color; class C public static method Main(): int var c: Color? := null; if c = null then return 1; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
