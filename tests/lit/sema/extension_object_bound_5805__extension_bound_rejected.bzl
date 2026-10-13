// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.338 / Appendix C.4, v0.338 / #5805 follow-up: an extension
// block's `where` clause is read under §14.2 (spec :5672, v0.338), so a bound
// naming `Object` on the block is a compile error at the declaration. The
// remedy is `where T: notnull` or an unconstrained block parameter.
//
// Fails-without-fix: before this change the extension block's `where` clause
// never reached the declaration pass (the walk covered only type and method
// declarations), so this program compiled with rc=0 and no diagnostic.

module M;

public extension IEnumerable<T> where T: Object
  public method Foo(): int => 1;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is constrained to `Object`, the root type
// CHECK-SAME: (§14.2)
