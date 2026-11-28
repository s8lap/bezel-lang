//! case: s31_2_3604
//! doc: spec
//! section: 31.2
//! title: a raise whose arm is left by a throw ends, so the next await of the task begins a new raise
//! rule: A raise of one of those instances ends when a catch arm that matched it is left by any edge other
//!   than a bare re-raise (`throw;`, which continues it), or when a supersession drops it from the
//!   in-flight slot
//! cites: 31.2, 30.9, 31.2.4
//! expect: run
//! output: 1
//!   C
//!   0
//! issue: 8640
//! notes: From the critic's distinguishing case, simplified: the first await's arm leaves by
//!   `throw new ExC`, which ends the raise, so the second await clears and prints 0. A rule under which
//!   only an arm that completes ends the raise would print 1 on the last line.
module Conf;

public class ExA: Exception
  public constructor(message: string) base(message);
  end constructor;
end ExA;

public class ExB: Exception
  public constructor(message: string) base(message);
  end constructor;
end ExB;

public class ExC: Exception
  public constructor(message: string) base(message);
  end constructor;
end ExC;

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
      try
        await t;
      catch
        var e: ExB => do
          Console.WriteLine(e.SupersededChain.Count);
          throw new ExC("c");
        end,
      end try;
    catch
      ExC => Console.WriteLine("C"),
    end try;
    try
      await t;
    catch
      var e: ExB => Console.WriteLine(e.SupersededChain.Count),
    end try;
    return 0;
  end Main;
end Program;
