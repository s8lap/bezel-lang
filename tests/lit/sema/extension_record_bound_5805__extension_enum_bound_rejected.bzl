// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.338 (constraint table, `where T: SomeClass` row) / §26.4,
// v0.338 / #5805 follow-up: an enum-typed bound on an extension block is a
// compile error at the declaration (the #4689 kind check, routed to extensions).
//
// Fails-without-fix: before this change the extension walk did not run the
// record/enum/choice rule, so this compiled with rc=0.

module M;

public enum Color Red, Green, Blue; end Color;

public extension IEnumerable<T> where T: Color
  public method Foo(): int => 1;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is constrained to `Color`, a enum
// CHECK-SAME: (§14.2)
