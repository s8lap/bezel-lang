//! case: s5_1_6716
//! doc: spec
//! section: 5.1
//! title: a conditional expression of interface type at a class-typed parameter is not implicitly convertible
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
  static method F(c: bool, i: IMark, a: A)
    Program.Log((if c then a else i));
  end F;
  public static method Main()
    Console.WriteLine("y");
  end Main;
end Program;
