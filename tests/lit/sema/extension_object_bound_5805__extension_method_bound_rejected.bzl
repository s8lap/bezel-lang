// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.338 / Appendix C.4, v0.338 / #5805 follow-up: the method-level
// form of the Object-bound rule on an extension. An extension member is read as
// a generic method (spec :5672), so its own `where` clause naming `Object` is a
// compile error at the member's declaration, independent of any call site.
//
// Fails-without-fix: before this change extension members' `where` clauses were
// never checked by the declaration pass, so this compiled with rc=0.

module M;

public extension IEnumerable<T>
  public method Count<U>(x: U): int where U: Object => 1;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `U` is constrained to `Object`, the root type
// CHECK-SAME: (§14.2)
