//! case: s5_1_6715
//! doc: spec
//! section: 5.1
//! title: an interface-typed lambda parameter passed at a class-typed parameter is not implicitly convertible
//! rule: The following conversions are implicit
//! cites: 12.7
//! expect: reject
//! diagnostic: implicit
module Conf;

public interface IMark
end IMark;

public class Program
  static method Log(ex: Exception)
    Console.WriteLine("x");
  end Log;
  public static method Main()
    var f := lambda(i: IMark) => Program.Log(i);
  end Main;
end Program;
