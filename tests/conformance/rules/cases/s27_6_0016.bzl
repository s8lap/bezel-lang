//! case: s27_6_0016
//! doc: spec
//! section: 27.6
//! title: a non-generic Math.Abs reference written with a type-argument list is rejected
//! rule: Overload selection follows the list rule: with a list, only candidates declaring that many type parameters of their own take part; without one, only candidates declaring none take part.
//! cites: 27.6, 14.5
//! expect: reject
//! diagnostic: type argument
//! issue: 5814
module Conf;

public class Program
  public static method Main()
    var f: (int) -> int := Math.Abs<int>;
    Console.WriteLine(f(-4));
  end Main;
end Program;
