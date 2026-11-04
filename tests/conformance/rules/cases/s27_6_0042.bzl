//! case: s27_6_0042
//! doc: spec
//! section: 27.6
//! title: a bound reference over an extension on Stack<int> captures the stack by reference
//! rule: the instance is captured at formation — by reference for class- or
//!   interface-typed receivers
//! cites: 27.6
//! expect: run
//! issue: 7488
//! output: 103
module Conf;

extension Stack<int>
  public method Length(): int => this.Count + 100;
end;

public class Program
  public static method Main()
    using var s := new Stack<int>();
    s.Push(1);
    s.Push(2);
    var f: () -> int := s.Length;
    s.Push(3);
    Console.WriteLine(f());
  end Main;
end Program;
