// fix #3840 (§10.4 / §35.2, v0.288): a static constructor is not part of
// §10.4's exhaustive admitted-member list — an enum has no per-instance
// state to initialize and no inheritance hierarchy; a choice has no user-visible
// constructor at all (§35.3 construction is synthetic).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; enum E A, B; public static constructor() end; end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum `E` may not declare a static constructor (§10.5)
