// Spec §30.7 (v0.184; spec header v0.196): POSITIVE control. The reject is
// yield-scoped and liveness-precise — it fires only when the `await using var`
// binding is LIVE at the `yield return`. Here the `TaskScope` is disposed at the
// end of its nested `do ... end` block, BEFORE the `yield return`, so it is not
// live at the yield and the program is accepted (no over-rejection).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System.Threading;
class Prog
  public async method Gen(): IAsyncEnumerable<int>
    do
      await using var scope := new TaskScope();
      scope.Spawn(async lambda() => do await Task.Delay(1); end);
      await scope.Join();
    end;
    yield return 1;
  end Gen;
end Prog;

// NODIAG-NOT: error
// NODIAG-NOT: §
