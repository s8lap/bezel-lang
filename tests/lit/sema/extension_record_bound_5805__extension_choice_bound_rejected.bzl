// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.338 (constraint table, `where T: SomeClass` row) / §26.4,
// v0.338 / #5805 follow-up: a choice-typed bound on an extension block is a
// compile error at the declaration (the #4689 kind check, routed to extensions).
//
// Fails-without-fix: before this change the extension walk did not run the
// record/enum/choice rule, so this compiled with rc=0.

module M;

public choice Opt Some(value: int), None; end Opt;

public extension IEnumerable<T> where T: Opt
  public method Foo(): int => 1;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is constrained to `Opt`, a choice
// CHECK-SAME: (§14.2)
