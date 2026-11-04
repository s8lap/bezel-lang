//! case: s5_1_0801
//! doc: spec
//! section: 5.1
//! title: a float argument converts implicitly to the double parameter of a function value in a collection
//! rule: | `float` | `double` |
//! cites: 5.1
//! expect: run
//! output: 1.5
//!   1.5
//! issue: 8144
module Conf;

public class Program
  public static method Main()
    var fs := new Array<(double) -> double>();
    fs.Add(lambda(x: double) => x + 1.0);
    var f: float := 0.5f;
    Console.WriteLine(fs[0](f));
    var d := new Dictionary<string, (double) -> double>();
    d["k"] := lambda(x: double) => x * 3.0;
    Console.WriteLine(d["k"](f));
  end Main;
end Program;
