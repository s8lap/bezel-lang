//! case: s27_6_0043
//! doc: spec
//! section: 27.6
//! title: a type-qualified reference to an extension method takes the receiver as its first parameter
//! rule: it captures no receiver, and the resulting function has the receiver
//!   as its first parameter
//! cites: 27.6
//! expect: run
//! issue: 7489
//! output: 42
module Conf;

extension Stack<int>
  public method Length(): int => 42;
end;

public class Program
  public static method Main()
    using var s := new Stack<int>();
    var f: (Stack<int>) -> int := Stack<int>.Length;
    Console.WriteLine(f(s));
  end Main;
end Program;
