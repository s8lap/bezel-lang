//! case: s5_1_0802
//! doc: spec
//! section: 5.1
//! title: a float argument converts implicitly to the double parameter of a method-reference function value
//! rule: | `float` | `double` |
//! cites: 5.1
//! expect: run
//! stdout: 5
//! issue: 8144
module Conf;

public class Program
  public static method Twice(x: double): double
    return x * 2.0;
  end Twice;
  public static method Main()
    var r: (double) -> double := Program.Twice;
    var f: float := 2.5f;
    var d := r(f);
    Console.WriteLine(d);
  end Main;
end Program;
