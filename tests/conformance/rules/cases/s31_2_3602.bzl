//! case: s31_2_3602
//! doc: spec
//! section: 31.2
//! title: a second await of a faulted task after the first handled it begins a new raise
//! rule: A re-raise of an instance that is propagating nowhere and bound by no arm, after an earlier raise
//!   of it has ended — the second awaiter of a multi-awaited task after the first handled the fault, a
//!   later touch of a failed type — begins a new raise and clears first.
//! cites: 31.2, 30.9, 30.4
//! expect: run
//! output: 1
//!   0
//! issue: 8640
module Conf;

public class ExA: Exception
  public constructor(message: string) base(message);
  end constructor;
end ExA;

public class ExB: Exception
  public constructor(message: string) base(message);
  end constructor;
end ExB;

public class Program
  public static async method Fail(): Task<int>
    try
      throw new ExA("first");
    finally
      throw new ExB("second");
    end try;
  end Fail;
  public static async method Main(): Task<int>
    var t := Program.Fail();
    try
      await t;
    catch
      var e: ExB => Console.WriteLine(e.SupersededChain.Count),
    end try;
    try
      await t;
    catch
      var e: ExB => Console.WriteLine(e.SupersededChain.Count),
    end try;
    return 0;
  end Main;
end Program;
