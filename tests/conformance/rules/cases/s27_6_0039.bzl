//! case: s27_6_0039
//! doc: spec
//! section: 27.6
//! title: a receiver-qualified reference to an extension method over Array<int> forms and calls it
//! rule: the reference is a bound reference: the receiver is captured at
//!   formation as the instance-method sentence above states
//! cites: 27.6
//! expect: run
//! issue: 7488
//! output: 42
module Conf;

extension Array<int>
  public method Length(): int => 42;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var f: () -> int := a.Length;
    Console.WriteLine(f());
  end Main;
end Program;
