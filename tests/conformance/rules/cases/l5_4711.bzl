//! case: l5_4711
//! doc: stdlib
//! section: 5
//! title: Count() on an IEnumerable<T> receiver reaches the stdlib 5 operator
//! rule: Count(): int;
//! cites: 5, 1
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [7, 8, 9];
    var c: IEnumerable<int> := a;
    Console.WriteLine(c.Count());
  end Main;
end Program;
