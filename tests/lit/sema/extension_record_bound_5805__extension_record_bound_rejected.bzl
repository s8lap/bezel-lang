// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.338 (constraint table, `where T: SomeClass` row; combination
// text) / §26.4, v0.338 / #5805 follow-up: an extension block's `where` clause
// is read under §14.2 (spec :5672), so a record-typed bound on the block is a
// compile error at the declaration, the same rule #4689 applies to a class
// declaration's `where T: SomeRecord`. The remedy is `where T: value`.
//
// Fails-without-fix: before this change the extension walk ran without
// kRuleRecordEnumChoice, so this program compiled with rc=0 and no diagnostic.

module M;

public record Point(X: int, Y: int);

public extension IEnumerable<T> where T: Point
  public method Foo(): int => 1;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is constrained to `Point`, a record
// CHECK-SAME: (§14.2)
