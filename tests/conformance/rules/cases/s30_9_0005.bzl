//! case: s30_9_0005
//! doc: spec
//! section: 30.9
//! title: the WithCleanupAsync example -- await in a catch arm and in a finally, then a bare re-raise
//! rule: `await` has no syntactic restrictions on its placement inside
//!   `try`/`catch`/`finally` *bodies*. The state machine handles suspension
//!   across exception-handling boundaries correctly. ... a bare `throw;`
//!   faults the body's task, and awaiting that task re-raises the exception.
//! cites: 30.9, 21.8
//! expect: run
//! output: logged bad
//!   released
//!   faulted: bad
module Conf;

public class Work
  public static async method DoWorkAsync(): Task
    await Task.Delay(0);
    throw new InvalidOperationException("bad");
  end DoWorkAsync;
  public static async method LogErrorAsync(m: string): Task
    await Task.Delay(0);
    Console.WriteLine("logged " + m);
  end LogErrorAsync;
  public static async method ReleaseResourcesAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("released");
  end ReleaseResourcesAsync;
  public static async method WithCleanupAsync(): Task
    try
      await DoWorkAsync();
    catch
      var ex: Exception => do
        await LogErrorAsync(ex.Message);
        throw;
      end
    finally
      await ReleaseResourcesAsync();
    end try;
  end WithCleanupAsync;
end Work;

public class Program
  public static async method Main(): Task
    try
      await Work.WithCleanupAsync();
      Console.WriteLine("unreached");
    catch
      var ex: InvalidOperationException => Console.WriteLine("faulted: " + ex.Message)
    end try;
  end Main;
end Program;
