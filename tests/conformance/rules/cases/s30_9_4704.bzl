//! case: s30_9_4704
//! doc: spec
//! section: 30.9
//! title: a borrowed task parameter operand is retained by borrow and the owner disposes the instance once
//! rule: A borrowed operand — a plain task parameter, a task read from a field or static storage
//! cites: 30.1, 30.5, 28.2
//! expect: run
//! stdout: 200 100 100
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

  public static async method V(t: Task): Task
    await t;
  end V;

  public static async method Main(): Task
    for var i in 0..<100 do
      var t := Program.BoomAsync();
      try
        await Program.V(t);
      catch var e: Exception =>
        XC.boom := XC.boom + 1;
      end;
      try
        await t;
      catch var e2: Exception =>
        XC.boom := XC.boom + 1;
      end;
    end for;
    Console.WriteLine($"{XC.boom} {XC.made} {XC.gone}");
  end Main;
end Program;
