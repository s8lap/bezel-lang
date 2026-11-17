//! case: s28_3_1_0002
//! doc: spec
//! section: 28.3.1
//! title: a task disposed on one branch only is released per path at scope
//!   end
//! rule: scope-end disposal is per-path
//! cites: 28.3.1, 28.2
//! expect: run
//! output: done
module Conf;

public class Program
  public static async method W(): Task
    await Task.Delay(1);
  end W;
  public static async method Run(i: int): Task
    var t := Program.W();
    await t;
    if i mod 2 = 0 then
      t.Dispose();
    end if;
  end Run;
  public static async method Main(): Task
    var i: int := 0;
    while i < 2 do
      await Program.Run(i);
      i := i + 1;
    end while;
    Console.WriteLine("done");
  end Main;
end Program;
