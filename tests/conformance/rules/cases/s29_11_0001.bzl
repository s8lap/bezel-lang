//! case: s29_11_0001
//! doc: spec
//! section: 29.11
//! title: iterating the same collection source twice with two for loops works as expected
//! rule: Each `GetEnumerator()` call produces an independent enumerator.
//!   Iterating the same source twice with two `for` loops works as expected
//!   for collection types.
//! cites: 29.11
//! expect: run
//! stdout: 12
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    var total := 0;
    for var x in xs do
      total := total + x;
    end;
    for var y in xs do
      total := total + y;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
