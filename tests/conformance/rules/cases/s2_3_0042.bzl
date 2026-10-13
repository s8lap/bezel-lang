//! case: s2_3_0042
//! doc: spec
//! section: 2.3
//! title: an auto-imported System.Text name is accepted
//! rule: The following names are visible in every source file without
//!   import; the standard exception types are among them (v0.338, §2.3).
//! cites: 2.3
//! expect: run
//! output: ok
module Conf;

public class B
  public constructor() end constructor;
  public method Take(s: System.Text.StringBuilder): int => 1;
end B;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
