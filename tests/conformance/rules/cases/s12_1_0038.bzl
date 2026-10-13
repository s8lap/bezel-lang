//! case: s12_1_0038
//! doc: spec
//! section: 12.1
//! title: an override may widen a built-in generic class parameter to T?
//! rule: An override may declare a parameter as `T?` where the overridden
//!   member declares `T`; visibility, return type and parameter types
//!   otherwise match the base member exactly (v0.338, §12.1).
//! cites: 12.1
//! expect: run
//! output: 2
module Conf;

public class B
  public constructor() end constructor;
  public virtual method Log(s: Stack<int>): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(s: Stack<int>?): int => 2;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var b: B := c;
    var s := new Stack<int>();
    Console.WriteLine(b.Log(s));
    return 0;
  end Main;
end Program;
