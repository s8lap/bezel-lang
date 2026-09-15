//! case: s29_2_0001
//! doc: spec
//! section: 29.2
//! title: the for variable over value-typed elements is a copy
//! rule: What the `for` variable is, decided by the static type of the
//!   source: value elements (`Array<int>`, a range, a string's chars) are a
//!   copy.
//! cites: 29.2
//! expect: run
//! stdout: 6
module Conf;

public class Program
  public static method Main()
    var arr: Array<int> := [1, 2, 3];
    for var x in arr do
      x := x + 100;
    end;
    var total := 0;
    for var y in arr do
      total := total + y;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
