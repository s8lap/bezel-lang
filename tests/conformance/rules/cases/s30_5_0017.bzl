//! case: s30_5_0017
//! doc: spec
//! section: 30.5
//! title: a Pattern A kind-1 slot -- the task owns its fresh owned result and disposes it
//! rule: Pattern A result slots ... 1. **A fresh owned value** ... the task
//!   **owns the result** and its destructor disposes it directly. "Disposing
//!   the task disposes the result": the result's lifetime is tied to the task.
//! cites: 30.5
//! expect: run
//! output: got
//!   res disposed
//!   scope end
module Conf;

public class Res
  destructor
    Console.WriteLine("res disposed");
  end destructor;
end Res;

public class Producer
  public static async method MakeAsync(): Task<Res>
    await Task.Delay(0);
    return new Res();
  end MakeAsync;
end Producer;

public class Program
  public static async method Main(): Task
    do
      using var t := Producer.MakeAsync();
      var r := await t;
      Console.WriteLine("got");
    end do;
    Console.WriteLine("scope end");
  end Main;
end Program;
