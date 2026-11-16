//! case: s30_11_0018
//! doc: spec
//! section: 30.11
//! title: returning the Task of an owned-field source from a method of the owner compiles
//! rule: A read of the property is a borrow of the task, sourced at the source object's location
//! cites: 30.11, 28.6
//! expect: run
//! output: ok
//! issue: 8616
module Conf;

import System.Threading;

public class Holder
  public owned Src: TaskCompletionSource<int> := new TaskCompletionSource<int>();

  public method Pending(): Task<int>
    return Src.Task;
  end Pending;
end Holder;

public class Program
  public static method Main()
    Console.WriteLine("ok");
  end Main;
end Program;
