// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.196 (a cycle among type-parameter bounds is a compile error at
// the declaration) and v0.338 (extension where clauses read under §14.2, spec
// :5672) / #5805 follow-up: `where T: U where U: T` on an extension block is a
// cycle among the block's own parameters.
//
// Fails-without-fix: before this change the extension block's cycle check never
// ran, so this compiled with rc=0 and no diagnostic.

module M;

class C<T, U> end C;

extension C<T, U> where T: U where U: T
  public method Foo(): int => 1;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cyclic constraint among type parameters
// CHECK-SAME: (§14.2)
