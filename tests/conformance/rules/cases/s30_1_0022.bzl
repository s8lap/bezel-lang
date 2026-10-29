//! case: s30_1_0022
//! doc: spec
//! section: 30.1
//! title: a Task<int> moved into an async method owned parameter is adopted by its frame and awaited
//! rule: An async method's `owned` parameters transfer into the state-machine frame at call time
//! cites: 30.1, 22.3
//! expect: run
//! output: 700
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
    var sum := 0;
    for var i in 0..<100 do
      var t := Program.WI();
      var r := await Program.TakeInt(move t);
      sum := sum + r;
    end;
    Console.WriteLine(sum);
  end Main;
end Program;
