//! case: s28_3_1_0001
//! doc: spec
//! section: 28.3.1
//! title: a settled task disposed explicitly is not disposed again at scope
//!   end
//! rule: Scope-end machinery skips its disposal
//! cites: 28.3.1, 28.2
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
    t.Dispose();
    Console.WriteLine("done");
  end Main;
end Program;
