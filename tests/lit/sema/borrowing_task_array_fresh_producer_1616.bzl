// REJECT test — issue #1616 (spec §15.4 / §30.5): a borrowing
// `Array<Task>` collection literal passed to `Task.WhenAll` / `Task.WhenAny`
// must not contain fresh Task-family producers. §15.4 admits only borrows
// as elements of borrowing-family collections; a fresh `Probe.Tick(7)` is
// a non-trivial-dispose, unbound owned temporary whose lifetime is the
// full expression — the borrowing array would orphan the caller's task
// frames at the end of the statement, exactly the original issue's leak
// family widened to the operator-level lint.
//
// History: #1616 was originally a CRASH (the `bz_task_when_all` call
// received null, leading to SIGSEGV or hang). The crash family was fixed
// by `lowerTaskArrayArg` mirroring `lowerTaskArrayArg`; that fix is
// reused here for both `WhenAll` and `WhenAny`. The new REJECT covers
// the still-open leak family: a `Probe.Tick(...)` placed inline is
// anonymous at the full-expression boundary and would orphan the caller's
// 184 B task frame even with the array-lowering fix. The diagnostic
// surfaces a precise spec citation and a clear fix-it.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrowing `Array<Task>` collection literal passed to `Task.WhenAll` may not contain a fresh Task-family producer
// CHECK-SAME: §15.4 / §30.5
module M;
class Probe
  public static async method Tick(n: int): Task
    await Task.Delay(0);
  end Tick;
end Probe;
class Program
  public static method Main(): int
    Task.WhenAll([Probe.Tick(1), Probe.Tick(2)]).Wait();
    return 0;
  end Main;
end Program;
