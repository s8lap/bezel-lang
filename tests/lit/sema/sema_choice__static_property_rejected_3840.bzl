// fix #3840 (§10.4 / §35.2, v0.288): a `static` property of any body kind
// is not part of §35.2's exhaustive admitted-member list — the list names
// only instance properties (expression-bodied read-only form); static members
// belong in the static tier.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; choice E A, B; public static property P: int => 0; end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: choice `E` may not declare a static property (§35.1 / §35.2)
