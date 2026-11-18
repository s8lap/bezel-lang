//! case: s5_1_6712
//! doc: spec
//! section: 5.1
//! title: an interface-typed argument at a class-typed parameter is not implicitly convertible
//! rule: The following conversions are implicit
//! cites: 12.7
//! expect: reject
//! diagnostic: implicit
module Conf;

public interface IMark
end IMark;

public class A: Exception, IMark
  public constructor(m: string) base(m); end constructor;
end A;

public class Program
  static method Log(ex: Exception)
    Console.WriteLine("x");
  end Log;
  static method F(i: IMark)
    Program.Log(i);
  end F;
  public static method Main()
    using var a := new A("m");
    Program.F(a);
  end Main;
end Program;
