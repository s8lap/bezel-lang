//! case: s2_3_0038
//! doc: spec
//! section: 2.3
//! title: a System.Exception parameter type is qualified without import
//! rule: The following names are visible in every source file without
//!   import; the standard exception types are among them (v0.338, §2.3).
//! cites: 2.3, 31.4
//! expect: run
//! output: 2
module Conf;

public class B
  public constructor() end constructor;
  public virtual method Log(e: System.Exception): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(e: System.Exception): int => 2;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var b: B := c;
    using var x := new System.Exception("x");
    Console.WriteLine(b.Log(x));
    return 0;
  end Main;
end Program;
