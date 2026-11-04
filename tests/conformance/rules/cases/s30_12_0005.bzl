//! case: s30_12_0005
//! doc: spec
//! section: 30.12
//! title: Mutex.TryAcquire(timeout) lexically inside an async lambda is rejected
//! rule: the compiler rejects the blocking forms lexically inside `async`
//!   methods and `async` lambdas (there the async forms are strictly superior
//!   — `await m.AcquireAsync()` / `await m.TryAcquireAsync(timeout)`)
//! cites: 30.12, 30.6
//! expect: reject
//! diagnostic: blocking
//! notes: accepting twin s30_12_0006 — the one edit is TryAcquire becoming
//!   `await … TryAcquireAsync`. The enclosing method is not async, so only
//!   the async-lambda row of the rule reaches the call.
module Conf;

import System.Threading;

public class Program
  public static method Make(): (Mutex) -> Task
    return async lambda(m: Mutex) => do
      using var g := m.TryAcquire(TimeSpan.Zero);
      await Task.Delay(0);
    end;
  end Make;
end Program;
