//! case: s28_2_4810
//! doc: spec
//! section: 28.2
//! title: disposing the owner after taking an alias that is never used again
//!   is admitted
//! rule: The kill itself stays permitted
//! cites: 28.2, 28.3.1
//! expect: run
//! stdout: done
module Conf;

public class Program
  public static async method W(): Task
    await Task.Delay(1);
  end W;
  public static async method Main(): Task
    var t := Program.W();
    await t;
    var b := t;
    t.Dispose();
    Console.WriteLine("done");
  end Main;
end Program;
