//! case: s30_12_0006
//! doc: spec
//! section: 30.12
//! title: TryAcquireAsync is the admitted acquisition form inside an async lambda
//! rule: the compiler rejects the blocking forms lexically inside `async`
//!   methods and `async` lambdas (there the async forms are strictly superior
//!   — `await m.AcquireAsync()` / `await m.TryAcquireAsync(timeout)`)
//! cites: 30.12, 30.6
//! expect: accept
//! notes: accepting twin of s30_12_0005.
module Conf;

import System.Threading;

public class Program
  public async method RunAsync(m: Mutex): Task
    var f := async lambda() => do
      using var g := await m.TryAcquireAsync(TimeSpan.Zero);
      await Task.Delay(0);
    end;
    await f();
  end RunAsync;
end Program;
