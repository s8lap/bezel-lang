//! case: s28_3_1_0004
//! doc: spec
//! section: 28.3.1
//! title: a fresh task disposed explicitly in a loop body is not disposed
//!   again at iteration end
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
    var j: int := 0;
    while j < 3 do
      var t := Program.W();
      await t;
      t.Dispose();
      j := j + 1;
    end while;
    Console.WriteLine("done");
  end Main;
end Program;
