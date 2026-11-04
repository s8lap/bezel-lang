//! case: s26_5_0072
//! doc: spec
//! section: 26.5
//! title: an extension method cannot be called through its extended type name
//! rule: No call-site syntax qualifies an extension's defining type, because
//!   extension blocks are nameless and extensions have no static-invocation
//!   form
//! cites: 26.5, 27.6
//! expect: reject
//! diagnostic: no member
//! issue: 7489
module Conf;

extension Array<int>
  public method Length(): int => 42;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(Array<int>.Length(a));
  end Main;
end Program;
