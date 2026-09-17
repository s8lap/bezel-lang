//! case: s30_9_0004
//! doc: spec
//! section: 30.9
//! title: await is unrestricted inside catch and finally bodies
//! rule: `await` has no syntactic restrictions on its placement inside
//!   `try`/`catch`/`finally` *bodies*. The state machine handles suspension
//!   across exception-handling boundaries correctly.
//! cites: 30.9
//! expect: accept
module Conf;

public class Worker
  public constructor()
  end constructor;

  public async method LogErrorAsync(ex: Exception): Task
    await Task.Delay(0);
  end LogErrorAsync;

  public async method WithCleanupAsync(): Task
    try
      throw new InvalidOperationException("x");
    catch
      var ex: Exception => do
        await LogErrorAsync(ex);
        throw;
      end
    end try;
  end WithCleanupAsync;
end Worker;
