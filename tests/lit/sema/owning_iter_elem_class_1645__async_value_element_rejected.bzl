// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression for #1645 (stdlib §4.13, v0.196).
//
// The async owning-enumeration interface `IOwningAsyncEnumerable<T>` also
// constrains `where T: class` (stdlib §4.13). An async iterator method
// declared to return `IOwningAsyncEnumerable<int>` (a VALUE element) must be
// rejected, exactly as the synchronous form is.
//
// FAIL-WITHOUT / PASS-WITH: before the fix sema accepted this ill-formed async
// iterator (the async lowering happened to avoid the sync `ICmp` crash, so it
// silently produced a program violating §4.13). With the fix sema reports the
// §4.13 diagnostic. // per stdlib §4.13

module M;
class Foo
  public async method Gen(): IOwningAsyncEnumerable<int>
    yield return 1;
  end Gen;
end Foo;

// CHECK: iterator method {{.*}}Gen{{.*}} returns {{.*}}IOwningAsyncEnumerable<T>{{.*}} but its element type {{.*}}int{{.*}} is not a class
// CHECK-SAME: (stdlib §4.13)
