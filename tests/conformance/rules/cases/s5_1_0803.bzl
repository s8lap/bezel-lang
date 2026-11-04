//! case: s5_1_0803
//! doc: spec
//! section: 5.1
//! title: a float argument converts implicitly to the double parameter of a function value returned by a call
//! rule: | `float` | `double` |
//! cites: 5.1
//! expect: run
//! stdout: 0.75
//! issue: 8144
module Conf;

public class Program
  public static method Make(): (double) -> double
    return lambda(x: double) => x + 0.25;
  end Make;
  public static method Main()
    var f: float := 0.5f;
    Console.WriteLine(Make()(f));
  end Main;
end Program;
