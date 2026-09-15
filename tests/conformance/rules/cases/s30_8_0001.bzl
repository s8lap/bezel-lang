//! case: s30_8_0001
//! doc: spec
//! section: 30.8
//! title: awaiting inside the token source's using var scope discharges the source-outlives obligation
//! rule: **The `CancellationTokenSource` must outlive the settlement of
//!   every task observing one of its tokens.** ... Awaiting the call inside
//!   the source's `using var` scope discharges the obligation.
//! cites: 30.8, 30.1
//! expect: accept
module Conf;

import System.Threading;

public class Program
  public async method WorkAsync(token: CancellationToken): Task
    token.ThrowIfCancellationRequested();
    await Task.Delay(0);
  end WorkAsync;

  public async method RunAsync(): Task
    using var cts := new CancellationTokenSource();
    await WorkAsync(cts.Token);
  end RunAsync;
end Program;
