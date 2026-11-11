//! case: s17_8_4303
//! doc: spec
//! section: 17.8
//! title: a chain of bang reads over a borrowed alias of an owned Task field stays a borrow of the field
//! rule: if `x` is borrowed, `x!` is borrowed with the same source
//! cites: 17.8, 28.3, 28.10
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
      using var h := new Holder();
      h.T := Program.W();
      var a := h.T!;
      var b := a!;
      var c := (h.T)!;
      await b;
    end for;
    Console.WriteLine("END");
  end Main;
end Program;
