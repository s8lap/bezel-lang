//! case: s30_5_0023
//! doc: spec
//! section: 30.5
//! title: an OwningTask moved into a using var and awaited yields one owned result disposed per iteration
//! rule: Awaiting transfers ownership to the awaiter
//! cites: 30.5, 28.10
//! expect: run
//! output: END 100
module Conf;

public class Res
  public static Count: int := 0;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;

public class Program
  public static async method W(): Task
    Console.WriteLine("w");
  end W;
  public static async method WO(): OwningTask<Res>
    return new Res();
  end WO;
  public static async method WI(): Task<int>
    return 7;
  end WI;
  public static async method TakeInt(owned x: Task<int>): Task<int>
    using var x2 := move x;
    return await x2;
  end TakeInt;

  public static async method Main(): Task
    for var i in 0..<100 do
      var t := Program.WO();
      await Task.Delay(1);
      using var u := move t;
      using var r := await u;
    end;
    Console.WriteLine("END " + Res.Count.ToString());
  end Main;
end Program;
