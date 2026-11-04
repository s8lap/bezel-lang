//! case: s30_12_0004
//! doc: spec
//! section: 30.12
//! title: Semaphore.Acquire() lexically inside an async method is rejected
//! rule: `Mutex.Acquire()`/`TryAcquire(timeout)`, `Semaphore.Acquire()`/`TryAcquire(timeout)`,
//!   … **block the calling thread**. … the compiler rejects the blocking forms
//!   lexically inside `async` methods and `async` lambdas
//! cites: 30.12, 30.3
//! expect: reject
//! diagnostic: blocking
//! notes: s30_12_0001 pins the Mutex form; this is the Semaphore row of the
//!   same list. Its admitted spelling is `await _gate.AcquireAsync()`.
module Conf;

import System.Threading;

public class Pool
  private owned _gate: Semaphore := new Semaphore(2);

  public async method UseAsync(): Task
    using var permit := _gate.Acquire();
    await Task.Delay(0);
  end UseAsync;
end Pool;
