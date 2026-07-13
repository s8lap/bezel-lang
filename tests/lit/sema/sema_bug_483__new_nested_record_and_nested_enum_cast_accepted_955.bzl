// Ported from tests/sema/bug_483_nested_type_construction_test.cpp
//   (BEZEL_TEST sema_bug_483, new_nested_record_and_nested_enum_cast_accepted_955).
//
// §9.5: `new Outer.Point(...)` (the correct form) and a nested enum cast
// `Outer.E(1)` remain accepted (no over-rejection of the nested branch).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; public class Outer public record Point(X: int, Y: int); public enum E A, B; end E; end Outer; public class Program public static method Main(): int var p := new Outer.Point(10, 20); var e: Outer.E := Outer.E(1); return p.X + int(e); end Main; end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
