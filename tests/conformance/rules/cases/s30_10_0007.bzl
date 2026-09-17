//! case: s30_10_0007
//! doc: spec
//! section: 30.10
//! title: a named using-var binding is the legal unsupervised abandonment form
//! rule: A named `using var t := call();` is legal (unsupervised): the task is
//!   disposed at scope end without blocking, and any exceptions it raises are
//!   silently swallowed.
//! cites: 30.5
//! expect: run
//! output: done
module Conf;

public class Program
  public static async method RunAsync(): Task
    await Task.Delay(0);
  end RunAsync;

  public static async method Main(): Task
    using var t := RunAsync();
    Console.WriteLine("done");
  end Main;
end Program;
