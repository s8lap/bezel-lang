//! case: s30_12_0002
//! doc: spec
//! section: 30.12
//! title: AcquireAsync is the admitted remedy for blocking acquisition inside async code
//! rule: the compiler rejects the blocking forms where the innermost enclosing method or lambda body is
//!   `async` (there the async forms are strictly superior — `await m.AcquireAsync()` /
//!   `await m.TryAcquireAsync(timeout)`)
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
