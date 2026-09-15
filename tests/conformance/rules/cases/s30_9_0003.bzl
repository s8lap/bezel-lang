//! case: s30_9_0003
//! doc: spec
//! section: 30.9
//! title: a catch guard may not contain await
//! rule: Catch **guards** are the one exception: a `when` guard on a catch
//!   arm is evaluated synchronously during exception dispatch and may not
//!   contain `await` (a compile error, in async and sync methods alike).
//! cites: 30.9, 19.4
//! expect: reject
//! diagnostic: guard
module Conf;

public class Worker
  public constructor()
  end constructor;

  public async method CheckAsync(): Task<bool>
    await Task.Delay(0);
    return true;
  end CheckAsync;
end Worker;

public class Program
  public async method RunAsync(): Task
    try
      throw new InvalidOperationException("x");
    catch
      var ex: InvalidOperationException when await new Worker().CheckAsync() => Console.WriteLine("caught")
    end try;
  end RunAsync;
end Program;
