//! case: s28_3_4309
//! doc: spec
//! section: 28.3
//! title: storing a plain Task local into an owned Task field inside a try disposes the task once
//! rule: store into an `owned` field or owning-collection slot, including through a `set(owned)` accessor
//! cites: 28.3.1, 31.2
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
      var t := Program.W();
      try
        h.T := t;
      catch
        var ex: Exception => Console.WriteLine("caught"),
      end try;
    end for;
    Console.WriteLine("END");
  end Main;
end Program;
