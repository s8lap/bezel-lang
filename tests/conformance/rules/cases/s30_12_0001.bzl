//! case: s30_12_0001
//! doc: spec
//! section: 30.12
//! title: Mutex.Acquire() lexically inside an async method is rejected
//! rule: the compiler rejects the blocking forms where the innermost enclosing method or lambda body is
//!   `async` (there the async forms are strictly superior — `await m.AcquireAsync()` /
//!   `await m.TryAcquireAsync(timeout)`)
//! cites: 30.12, 30.3
//! expect: reject
//! diagnostic: blocking
module Conf;

import System.Threading;

public class Account
  private owned _mutex: Mutex := new Mutex();

  public constructor()
  end constructor;

  public async method DepositAsync(): Task
    using var guard := _mutex.Acquire();
    await Task.Delay(0);
  end DepositAsync;
end Account;
