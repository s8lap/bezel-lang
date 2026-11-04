//! case: s27_6_0041
//! doc: spec
//! section: 27.6
//! title: a bound reference over an extension captures a class-typed receiver by reference
//! rule: the instance is captured at formation — by reference for class- or
//!   interface-typed receivers
//! cites: 27.6
//! expect: run
//! issue: 7488
//! output: 40
module Conf;

extension Array<int>
  public method Length(): int => this.Count * 10;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var f: () -> int := a.Length;
    a.Add(4);
    Console.WriteLine(f());
  end Main;
end Program;
