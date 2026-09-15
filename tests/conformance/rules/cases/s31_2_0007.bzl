//! case: s31_2_0007
//! doc: spec
//! section: 31.2
//! title: return inside a finally block is rejected
//! rule: `return` inside a `finally` block is a compile error (§21.5, §21.7).
//! cites: 31.2, 21.5, 21.7
//! expect: reject
//! diagnostic: return
module Conf;

public class Program
  public static method Compute(): int
    try
      DoWork();
    finally
      return 0;
    end try;
  end Compute;

  public static method DoWork()
  end DoWork;

  public static method Main()
    Console.WriteLine(Compute());
  end Main;
end Program;
