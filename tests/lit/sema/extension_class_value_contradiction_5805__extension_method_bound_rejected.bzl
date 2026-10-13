// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.338 / #5805 follow-up: the member-level form of the
// class/value contradiction on an extension. An extension member is read as a
// generic method (spec :5672, v0.338) that declares its own parameters, so a
// member whose own `where` clause requires both `class` and `value` on one
// parameter is a compile error at the declaration.
//
// Fails-without-fix: before this change extension members' `where` clauses were
// not checked by the declaration pass, so this compiled with rc=0.

module M;

public extension IEnumerable<T>
  public method Count<U>(x: U): int where U: class, value => 1;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: contradictory constraints on type parameter `U`: `class` and `value` cannot both be required
// CHECK-SAME: (§14.2)
