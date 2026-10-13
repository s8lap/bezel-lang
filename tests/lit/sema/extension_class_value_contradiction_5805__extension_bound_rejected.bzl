// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.338 / #5805 follow-up: a constraint combination that admits no
// type (`class, value`) is a compile error at the declaration (§14.2). An
// extension block's `where` clause is read under §14.2 (spec :5672, v0.338), so
// the contradiction is rejected on the block as it is on a type declaration.
//
// Fails-without-fix: before this change the extension block's `where` clause
// never reached the declaration pass's class/value check, so this compiled with
// rc=0 and no diagnostic.

module M;

public extension IEnumerable<T> where T: class, value
  public method Foo(): int => 1;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: contradictory constraints on type parameter `T`: `class` and `value` cannot both be required
// CHECK-SAME: (§14.2)
