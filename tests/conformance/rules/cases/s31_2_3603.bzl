//! case: s31_2_3603
//! doc: spec
//! section: 31.2
//! title: a return-await wrapper's awaiter sees the chain the inner body recorded
//! rule: The same holds through every task the fault escapes into unhandled — a `return await t;`
//!   wrapper's awaiter sees what `t`'s body recorded.
//! cites: 31.2, 30.9
//! expect: run
//! stdout: 1
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
  public static async method Wrap(t: Task<int>): Task<int>
    return await t;
  end Wrap;
  public static async method Main(): Task<int>
    try
      await Program.Wrap(Program.Fail());
    catch
      var e: ExB => Console.WriteLine(e.SupersededChain.Count),
    end try;
    return 0;
  end Main;
end Program;
