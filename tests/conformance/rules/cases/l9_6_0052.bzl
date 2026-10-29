//! case: l9_6_0052
//! doc: stdlib
//! section: 9.6
//! title: string Insert binds named startIndex and value in either order, or a named value after an index
//! rule: public pure method Insert(startIndex: int, value: string): string; ... `Insert(startIndex, value)` yields the receiver with `value`'s code points inserted at position `startIndex`
//! cites: 9.6, 22.5
//! expect: run
//! output: helZZlo world
//!   helYlo world
module Conf;

public class Program
  public static method Main()
    var s := "hello world";
    Console.WriteLine(s.Insert(value := "ZZ", startIndex := 3));
    Console.WriteLine(s.Insert(3, value := "Y"));
  end Main;
end Program;
