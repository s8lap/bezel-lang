// fix #3840 (§10.4 / §35.2, v0.288): a mutable static field is not part of
// §35.2's exhaustive admitted-member list — same as the enum case.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; choice E A, B; public static _x: int := 0; end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: choice `E` may not declare a mutable static field (§35.1 / §35.2)
