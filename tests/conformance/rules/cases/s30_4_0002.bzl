//! case: s30_4_0002
//! doc: spec
//! section: 30.4
//! title: a second await of an OwningTask<T> raises InvalidOperationException
//! rule: A `OwningTask<T>` is single-consumer per the Pattern B rules: the
//!   first await spends the task regardless of outcome, and a second await
//!   raises `InvalidOperationException` on the second await even when the
//!   first await completed successfully.
//! cites: 30.4, 30.5
//! expect: run
//! stdout: caught
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static async method Main(): Task
    using var t := Task.FromOwnedResult<Widget>(new Widget());
    using var w1 := await t;
    try
      using var w2 := await t;
    catch
      var ex: InvalidOperationException => Console.WriteLine("caught")
    end try;
  end Main;
end Program;
