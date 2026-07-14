// Spec §30.7 (v0.184; spec header v0.196): POSITIVE control. For a class that
// implements `IAsyncDisposable` and DOES declare a `destructor`, §28.12's
// synchronous-fallback path on consumer abandonment remains the defined degraded
// behavior — so such a binding IS allowed live across a `yield return`. The
// reject must not fire here (no over-rejection of the with-destructor shape).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
class AsyncRes: IAsyncDisposable
  public async method DisposeAsync(): Task
  end DisposeAsync;
  destructor end destructor;
end AsyncRes;
class Prog
  public async method Gen(): IAsyncEnumerable<int>
    await using var r := new AsyncRes();
    yield return 1;
  end Gen;
end Prog;

// NODIAG-NOT: error
// NODIAG-NOT: §
