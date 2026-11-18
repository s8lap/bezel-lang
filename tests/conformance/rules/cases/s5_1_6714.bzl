//! case: s5_1_6714
//! doc: spec
//! section: 5.1
//! title: an interface-typed generic value at a class-typed parameter is not implicitly convertible
//! rule: The following conversions are implicit
//! cites: 12.7
//! expect: reject
//! diagnostic: implicit
module Conf;

public interface IBox<T>
end IBox;

public class Program
  static method Log(ex: Exception)
    Console.WriteLine("x");
  end Log;
  static method F<T>(b: IBox<T>)
    Program.Log(b);
  end F;
  public static method Main()
    Console.WriteLine("y");
  end Main;
end Program;
