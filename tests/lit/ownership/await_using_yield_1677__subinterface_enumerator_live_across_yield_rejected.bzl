// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §30.7 (v0.184; spec header v0.196): the reject also covers an `await
// using var` of INTERFACE static type — `IAsyncDisposable` or any interface
// deriving from it (here `IAsyncEnumerator<int>`, which extends
// `IAsyncDisposable`, stdlib §4.13): an interface cannot promise a `destructor`,
// so the runtime type's synchronous-fallback posture is statically unknowable
// and interface-typed bindings are conservatively included. Holding such an
// enumerator live across a `yield return` is a compile error naming the binding.
module M;
class Prog
  public async method Inner(): IAsyncEnumerable<int>
    yield return 1;
  end Inner;
  public async method Outer(): IAsyncEnumerable<int>
    var src := Inner();
    await using var e := src.GetAsyncEnumerator();
    yield return 2;
  end Outer;
end Prog;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: await-using local `e` is held live across a `yield return`
// CHECK-SAME: (§30.7
