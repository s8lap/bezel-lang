//! case: s30_9_4705
//! doc: spec
//! section: 30.9
//! title: a using var operand's disposal is deferred to the faulted task's teardown
//! rule: A `using var`/`await using var` operand's disposal is deferred to the faulted task's teardown
//! cites: 30.5, 28.10
//! expect: run
//! stdout: 100 100 100
module Conf;

public class XC
  public static made: int := 0;
  public static gone: int := 0;
  public static boom: int := 0;
  public constructor() end constructor;
end XC;

public class MyEx: Exception
  public constructor(m: string) base(m); XC.made := XC.made + 1; end constructor;
  destructor XC.gone := XC.gone + 1; end destructor;
end MyEx;

public class Program
  public static async method BoomAsync(): Task
    await Task.Delay(1);
    throw new MyEx("boom");
  end BoomAsync;

  public static async method R(i: int): Task
    using var t := Program.BoomAsync();
    await t;
  end R;

  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        XC.boom := XC.boom + 1;
      end;
    end for;
    Console.WriteLine($"{XC.boom} {XC.made} {XC.gone}");
  end Main;
end Program;
