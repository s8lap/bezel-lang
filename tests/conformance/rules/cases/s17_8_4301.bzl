//! case: s17_8_4301
//! doc: spec
//! section: 17.8
//! title: the bang store of a nullable Task local into an owned field consumes it like the bare store
//! rule: `sink._f := x!;` consumes `x` exactly as `sink._f := x;` does.
//! cites: 17.8, 28.3.1
//! expect: run
//! stdout: END
//! issue: 7881
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
      using var h := new Holder();
      var t: Task? := Program.W();
      h.T := t!;
    end for;
    Console.WriteLine("END");
  end Main;
end Program;
