//! case: s30_9_4703
//! doc: spec
//! section: 30.9
//! title: four concurrent frames each awaiting an unbound faulting operand dispose each instance once
//! rule: moves into the faulted task's state as a kind-2 chained source does
//! cites: 30.5, 30.1
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
    await Program.BoomAsync();
  end R;

  public static async method Main(): Task
    for var i in 0..<25 do
      var t1 := Program.R(i);
      var t2 := Program.R(i + 1);
      var t3 := Program.R(i + 2);
      var t4 := Program.R(i + 3);
      try await t1; catch var e1: Exception => XC.boom := XC.boom + 1; end;
      try await t2; catch var e2: Exception => XC.boom := XC.boom + 1; end;
      try await t3; catch var e3: Exception => XC.boom := XC.boom + 1; end;
      try await t4; catch var e4: Exception => XC.boom := XC.boom + 1; end;
    end for;
    Console.WriteLine($"{XC.boom} {XC.made} {XC.gone}");
  end Main;
end Program;
