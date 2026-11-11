//! case: s28_3_4311
//! doc: spec
//! section: 28.3
//! title: owned Task parameters stored bare and with a bang into an owned Task field are consumed once
//! rule: store into an `owned` field or owning-collection slot, including through a `set(owned)` accessor
//! cites: 28.3.1, 28.1, 17.8
//! expect: run
//! stdout: END
module Conf;

public class Holder
  public owned T: Task? := null;
  public constructor() end constructor;

  public method Put(owned x: Task)
    this.T := x;
  end Put;

  public method PutB(owned x: Task?)
    this.T := x!;
  end PutB;
end Holder;

public class Program

  public static async method W(): Task
    await Task.Delay(1);
  end W;

  public static async method Main(): Task
    for var i in 0..<3 do
      using var h := new Holder();
      h.Put(Program.W());
      h.PutB(Program.W());
    end for;
    Console.WriteLine("END");
  end Main;
end Program;
