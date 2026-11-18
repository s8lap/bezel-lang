// #7435 (§28.2, v0.357): the repeated-disposal exemption names BOTH spellings.
//
//   "One use is exempt: a second `Dispose()` or `DisposeAsync()`, or a second
//    discard, of the same binding is the runtime no-op the carve-out above
//    defines and draws a compile-time warning"
//
// A second `await a.DisposeAsync()` is therefore the *disposed again* WARNING,
// never the use-after-dispose compile error, and the FIRST one is no use of a
// disposed binding at all. Before #7435 the callee read of a `DisposeAsync()`
// receiver was walked as a READ: the first await drew the *use of `a` after a
// dominating explicit Dispose()* text against its own kill, and so did the
// second. With that text promoted to an error, an un-exempted `DisposeAsync`
// would reject this legal program (rule-corpus row s28_12_0006 is this shape).
//
// ACCEPT-WITH-WARNING: bezelc -c must exit zero, and the one diagnostic is
// pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; import System;
public sealed class R: IAsyncDisposable
  public constructor()
  end constructor;
  public async method DisposeAsync(): Task
  end DisposeAsync;
end R;
public class C
  public static async method Body(): Task<int>
    var a := new R();
    await a.DisposeAsync();
    await a.DisposeAsync();
    return 1;
  end Body;
end C;

// CHECK-NOT: use of `a` after a dominating explicit Dispose() call
// CHECK: :32:{{[0-9]+}}:{{ ?warning: ?}} local `a` is disposed again here
// CHECK-SAME: (disposal is idempotent, §28.2)
// CHECK-NOT: use of `a` after a dominating explicit Dispose() call
