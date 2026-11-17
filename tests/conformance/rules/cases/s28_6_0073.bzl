//! case: s28_6_0073
//! doc: spec
//! section: 28.6
//! title: the null-forgiving explicit disposal of a nullable task is a
//!   disposal
//! rule: the `x?.Dispose()` / `x!.Dispose()` spellings
//! cites: 28.6, 28.3.1
//! expect: run
//! stdout: done
module Conf;

public class Program
  public static async method W(): Task
    await Task.Delay(1);
  end W;
  public static async method Main(): Task
    var t: Task? := Program.W();
    await Task.Delay(2);
    t!.Dispose();
    Console.WriteLine("done");
  end Main;
end Program;
