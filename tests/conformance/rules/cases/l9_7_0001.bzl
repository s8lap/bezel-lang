//! case: l9_7_0001
//! doc: stdlib
//! section: 9.7
//! title: Contains on an Array<T> receiver resolves to the instance member
//! rule: the LINQ extensions in §5 apply to `Array<T>` because it implements
//!   `IEnumerable<T>` — except the four whose names `Array<T>` itself declares:
//!   `Contains`, `Count`, `Reverse`, and `ToArray` resolve to the instance
//!   members and the same-named §5 operators are unreachable on an `Array<T>`
//!   receiver.
//! cites: 9.7, 4.4
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var xs: Array<string> := ["a", "b"];
    Console.WriteLine(xs.Contains("a"));
  end Main;
end Program;
