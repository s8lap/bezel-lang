//! case: s31_2_3601
//! doc: spec
//! section: 31.2
//! title: the first await of a faulted task delivers the supersession its async body recorded
//! rule: So a task-owned fault (§30.9) escaping the async body that raised it, and its re-raise by the
//!   task's first awaiter — an `await`, a `Result` read, or a combinator re-raising it — are one raise, and
//!   that re-raise clears nothing
//! cites: 31.2, 30.9, 31.4
//! expect: run
//! stdout: 1
//! issue: 8640
//! notes: The issue's program. ExB supersedes ExA in Fail's finally, so ExB's chain holds ExA.
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
    try
      await Program.Fail();
    catch
      var e: ExB => Console.WriteLine(e.SupersededChain.Count),
    end try;
    return 0;
  end Main;
end Program;
