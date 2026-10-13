// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.196 (self-referential bound) and v0.338 (extension where
// clauses read under §14.2, spec :5672) / #5805 follow-up: `where T: T` on an
// extension block is a compile error at the declaration.
//
// Fails-without-fix: before this change the extension block's `where` clause
// never reached the declaration pass's self-reference check, so this compiled
// with rc=0 and no diagnostic.

module M;

public extension IEnumerable<T> where T: T
  public method Foo(): int => 1;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: self-referential constraint on type parameter `T`
// CHECK-SAME: (§14.2)
