//! case: s28_3_1_0003
//! doc: spec
//! section: 28.3.1
//! title: a running task disposed on one branch and awaited on the other is
//!   released per path
//! rule: scope-end disposal is per-path
//! cites: 28.3.1, 28.2
//! expect: run
//! stdout: done
module Conf;

public class Program
  public static async method W(): Task
    await Task.Delay(1);
  end W;
  public static async method Run(i: int): Task
    var t := Program.W();
    if i mod 2 = 0 then
      t.Dispose();
    else
      await t;
    end if;
    await Task.Delay(2);
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
