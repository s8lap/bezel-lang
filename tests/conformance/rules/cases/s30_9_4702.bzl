//! case: s30_9_4702
//! doc: spec
//! section: 30.9
//! title: every awaiter of a multi-awaited faulted task re-raises the same live instance, disposed once
//! rule: every awaiter of a multi-awaited faulted task ([§30.4](#sec-multiple-awaiters)) re-raises the
//!   same live instance
//! cites: 30.4, 31.1
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

  public static async method Main(): Task
    for var i in 0..<100 do
      var t := Program.BoomAsync();
      try
        await t;
      catch var e1: MyEx =>
        XC.boom := XC.boom + 1;
      end;
      try
        await t;
      catch var e2: MyEx =>
        XC.boom := XC.boom + 1;
      end;
    end for;
    Console.WriteLine($"{XC.boom} {XC.made} {XC.gone}");
  end Main;
end Program;
