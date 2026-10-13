//! case: s2_3_0040
//! doc: spec
//! section: 2.3
//! title: a System.Exception owned return type is qualified without import
//! rule: The following names are visible in every source file without
//!   import; the standard exception types are among them (v0.338, §2.3).
//! cites: 2.3, 31.4
//! expect: run
//! output: ok
module Conf;

public class Program
  public static method Make(): owned System.Exception => new System.Exception("x");
  public static method Main(): int
    using var x := Make();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
