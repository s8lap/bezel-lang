// REJECT test — dictionary-entry element-borrow Dispose, retired with A8 (#2081).
//
// Spec §28.6/§28.11 (v0.213) retires axiom A8 (the transit exception): only an
// owner disposes. Iterating a borrowing `Dictionary<K, V>` yields entry borrows;
// `entry.Value` is a borrow of the value the caller does not own (the dictionary
// is a transit point). The pre-v0.213 asymmetric-ownership idiom
// `entry.Value.Dispose()` — the spec's own former asymmetric-dictionary example —
// is now a compile error. The migrated form declares `OwningDictionary<K, V>`
// (whose `Dispose()`/`Clear()` cascades to the values); see
// tests/conformance/cases/spec_0215.bzl.
//
// This is an OWNERSHIP-pass rejection (peeling the `entry.Value` member chain to
// its dictionary-iteration base), so only the real `bezelc` observes it.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
module M;
class Resource public method Ping(): () end Ping; end Resource;
class Program
  public static method Main(): int
    using var d: Dictionary<int, Resource> := new Dictionary<int, Resource>();
    d[1] := new Resource();
    for var entry in d do
      entry.Value.Dispose();
    end;
    d.Clear();
    return 0;
  end Main;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot call `Dispose()` on an element borrow read from a borrowing-family collection
// CHECK-SAME: §28.11
// CHECK-SAME: axiom A8
