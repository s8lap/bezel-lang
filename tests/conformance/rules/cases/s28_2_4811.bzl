//! case: s28_2_4811
//! doc: spec
//! section: 28.2
//! title: a repeated null-conditional explicit disposal of a task is a
//!   runtime no-op
//! rule: subsequent calls are no-ops
//! cites: 28.2, 28.3.1
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
    t?.Dispose();
    t?.Dispose();
    Console.WriteLine("done");
  end Main;
end Program;
