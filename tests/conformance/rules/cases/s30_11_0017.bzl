//! case: s30_11_0017
//! doc: spec
//! section: 30.11
//! title: returning a parameter source's Task is a borrow that outlives the call and compiles
//! rule: A read of the property is a borrow of the task, sourced at the source object's location
//! cites: 30.11, 28.3
//! expect: run
//! output: ok
//! issue: 8616
module Conf;

import System.Threading;

public class Program
  public static method Pick(tcs: TaskCompletionSource<int>): Task<int>
    return tcs.Task;
  end Pick;

  public static method Main()
    Console.WriteLine("ok");
  end Main;
end Program;
