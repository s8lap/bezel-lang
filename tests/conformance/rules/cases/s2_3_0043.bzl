//! case: s2_3_0043
//! doc: spec
//! section: 2.3
//! title: an auto-imported System.Collections.Generic name is accepted
//! rule: The following names are visible in every source file without
//!   import; the standard exception types are among them (v0.338, §2.3).
//! cites: 2.3
//! expect: run
//! output: ok
module Conf;

public class B
  public constructor() end constructor;
  public method Take(d: System.Collections.Generic.Dictionary<string, int>): int => 1;
end B;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
