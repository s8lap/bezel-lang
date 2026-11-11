//! case: s17_8_4304
//! doc: spec
//! section: 17.8
//! title: a bang read of an owned Task local binds a borrow alias that is read like the bare read
//! rule: an unmarked `x!` is read as an unmarked `x` is
//! cites: 17.8, 28.3, 30.5
//! expect: run
//! stdout: END
//! issue: 7808
module Conf;

public class Holder
  public owned T: Task? := null;
  public constructor() end constructor;
end Holder;

public class Program
  public static async method W(): Task
    await Task.Delay(1);
  end W;

  public static async method Main(): Task
    for var i in 0..<3 do
      var t: Task? := Program.W();
      var u := t!;
      await u;
    end for;
    Console.WriteLine("END");
  end Main;
end Program;
