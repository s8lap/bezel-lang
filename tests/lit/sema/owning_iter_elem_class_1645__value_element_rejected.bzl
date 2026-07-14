// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression for #1645 (stdlib §4.13, v0.196).
//
// The owning-enumeration interfaces `IOwningEnumerable<T>` /
// `IOwningEnumerator<T>` (and the async pair) each constrain `where T: class`
// (stdlib §4.13) — iteration transfers OWNERSHIP of each element, which is
// meaningful only for a disposable class value. An iterator method (a body
// with `yield`) declared to return `IOwningEnumerable<int>` (a VALUE element)
// must therefore be rejected.
//
// FAIL-WITHOUT / PASS-WITH: before the fix sema accepted this ill-formed
// program and codegen then hit an `ICmp type-mismatch` assertion in the
// owning-iterator lowering (bezelc aborted). With the fix sema reports the
// §4.13 diagnostic below and exits non-zero cleanly. // per stdlib §4.13

module M;
class Foo
  public method Gen(): IOwningEnumerable<int>
    yield return 1;
  end Gen;
end Foo;

// CHECK: iterator method {{.*}}Gen{{.*}} returns {{.*}}IOwningEnumerable<T>{{.*}} but its element type {{.*}}int{{.*}} is not a class
// CHECK-SAME: (stdlib §4.13)
