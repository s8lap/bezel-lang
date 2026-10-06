// fix #3840 (§10.4 / §35.2, v0.288): a static constructor is not part of
// §35.2's exhaustive admitted-member list — a choice has no user-visible
// constructor at all (§35.3 construction is synthetic).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; choice E A, B; public static constructor() end; end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: choice `E` may not declare a static constructor (§35.1 / §35.2)
