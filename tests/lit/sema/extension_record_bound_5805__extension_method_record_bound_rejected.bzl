// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.338 / §26.4, v0.338 / #5805 follow-up: the member form of the
// record-bound rule. An extension member is read as a generic method declaring
// the block's parameters before its own (spec :5672), so a member's own `where`
// clause naming a record is a compile error at the member's declaration.
//
// Fails-without-fix: before this change extension members were checked without
// kRuleRecordEnumChoice, so this compiled with rc=0.

module M;

public record Point(X: int, Y: int);

public extension IEnumerable<T>
  public method Count<U>(x: U): int where U: Point => 1;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `U` is constrained to `Point`, a record
// CHECK-SAME: (§14.2)
