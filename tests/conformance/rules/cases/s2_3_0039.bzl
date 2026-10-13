//! case: s2_3_0039
//! doc: spec
//! section: 2.3
//! title: a System.Exception local annotation and constructor are qualified without import
//! rule: The following names are visible in every source file without
//!   import; the standard exception types are among them (v0.338, §2.3).
//! cites: 2.3, 31.4
//! expect: run
//! output: ok
module Conf;

public class Program
  public static method Main(): int
    using var x: System.Exception := new System.Exception("x");
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
