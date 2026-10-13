// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.196 / v0.338 (spec :5672) / #5805 follow-up: the member-level
// self-reference on an extension. The member's own `where U: U` is a
// self-referential bound at the member's declaration, independent of any call.
//
// Fails-without-fix: before this change extension members' `where` clauses were
// not checked by the declaration pass's self-reference check; rc=0.

module M;

public extension IEnumerable<T>
  public method Count<U>(x: U): int where U: U => 1;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: self-referential constraint on type parameter `U`
// CHECK-SAME: (§14.2)
