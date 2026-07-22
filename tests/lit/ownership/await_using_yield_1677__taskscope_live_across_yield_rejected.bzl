// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail — see
// tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Spec §30.7 (v0.184; spec header v0.196): an `await using var` whose static
// type is a class implementing `IAsyncDisposable` with NO `destructor` — no
// synchronous fallback at all, by design (`TaskScope`, stdlib §6.10, is the
// standard instance) — MAY NOT be live across a `yield return` in an async
// iterator. On consumer abandonment at the yield suspension, §28.12's
// synchronous-fallback path runs and `DisposeAsync()` is skipped, silently
// voiding the type's cleanup. The reject is emitted at the yield, naming the
// binding.
//
// THE STRUCTURAL POINT (issue #1677): before the fix bezelc ACCEPTED this
// program (the sema-accept bug); the ownership pass never rejected the held
// `TaskScope`. This reject is emitted by the OWNERSHIP pass, which runs after
// sema — lit invokes the real `bezelc`, so the §30.7 diagnostic is always in the
// observed output. This is the pinned failing test: it fails-without the fix
// (bezelc exits 0, so `%not` fails) and passes-with it.
module M;
import System.Threading;
class Prog
  public async method Gen(): IAsyncEnumerable<int>
    await using var scope := new TaskScope();
    scope.Spawn(async lambda() => do await Task.Delay(2); end);
    yield return 1;
    await scope.Join();
  end Gen;
end Prog;

// The ownership pass names the binding (`scope`) and cites the rule (§30.7),
// not a generic "some error". bezelc emits diagnostics as `file:line:col:
// message` with no "error:" severity word, so we anchor on the `:line:col:`
// shape, the binding name, and the rule citation.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: await-using local `scope` is held live across a `yield return`
// CHECK-SAME: (§30.7
