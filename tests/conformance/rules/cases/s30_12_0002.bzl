//! case: s30_12_0002
//! doc: spec
//! section: 30.12
//! title: AcquireAsync is the admitted remedy for blocking acquisition inside async code
//! rule: The compiler rejects the blocking forms lexically inside `async`
//!   methods and `async` lambdas (there the async forms are strictly
//!   superior — `await m.AcquireAsync()`).
//! cites: 30.12
//! expect: accept
module Conf;

import System.Threading;

public class Account
  private owned _mutex: Mutex := new Mutex();

  public constructor()
  end constructor;

  public async method DepositAsync(): Task
    using var guard := await _mutex.AcquireAsync();
    await Task.Delay(0);
  end DepositAsync;
end Account;
