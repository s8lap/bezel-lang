//! case: s30_8_0004
//! doc: spec
//! section: 30.8
//! title: a token completed into Task.FromResult's slot is observed after settlement
//! rule: a token completed into a task's result slot —
//!   `TaskCompletionSource<CancellationToken>.SetResult`,
//!   `Task.FromResult(token)` (§30.5's exception) — is observed after
//!   settlement for as long as the task lives, and through every awaiter's
//!   copy after that
//! cites: 30.8, 30.5
//! expect: run
//! stdout: True
//! notes: the source is declared before the task, so it outlives the task and
//!   the awaited copy, discharging the obligation the same sentence states.
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    using var cts := new CancellationTokenSource();
    using var t := Task.FromResult(cts.Token);
    cts.Cancel();
    var tok := await t;
    Console.WriteLine(tok.IsCancellationRequested);
  end Main;
end Program;
