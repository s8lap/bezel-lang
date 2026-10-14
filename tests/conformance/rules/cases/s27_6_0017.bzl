//! case: s27_6_0017
//! doc: spec
//! section: 27.6
//! title: a Math.Max reference with two type arguments for one type parameter is rejected
//! rule: An explicit type-argument list is all-or-nothing: it must supply exactly as many arguments as the method declares type parameters. A shorter or a longer list is a compile error.
//! cites: 27.6, 14.5
//! expect: reject
//! diagnostic: type arguments
//! issue: 5814
module Conf;

public class Program
  public static method Main()
    var m: (int, int) -> int := Math.Max<int, int>;
    Console.WriteLine(m(3, 5));
  end Main;
end Program;
