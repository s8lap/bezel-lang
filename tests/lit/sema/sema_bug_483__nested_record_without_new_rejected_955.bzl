// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/bug_483_nested_type_construction_test.cpp
//   (BEZEL_TEST sema_bug_483, nested_record_without_new_rejected_955).
//
// §9.5 (#955): a NESTED record/class called WITHOUT `new` (`Outer.Point(1,2)`)
// is not construction and must be rejected with the same diagnostic as a
// top-level record/class — it previously slipped past (the NestedTypeSymbol
// callee misses the TypeSymbol-based §9.5 check) and miscompiled.

module M; public class Outer public record Point(X: int, Y: int); end Outer; public class Program public static method Main(): int var p := Outer.Point(10, 20); return p.X; end Main; end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `Point` is constructed with `new Point(...)`, not by calling the type name
// CHECK-SAME: (§9.5)
