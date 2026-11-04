//! case: s26_5_0071
//! doc: spec
//! section: 26.5
//! title: a type-qualified reference is refused when the extension block extends a different type argument
//! rule: Where neither rule 1 nor this search supplies the name, nothing
//!   supplies it: the access names no member and is a compile error at the
//!   name, the diagnostic naming the receiver's static type
//! cites: 26.5, 27.6
//! expect: reject
//! diagnostic: no member
//! issue: 7489
module Conf;

extension Array<string>
  public method Length(): int => 42;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var f: (Array<int>) -> int := Array<int>.Length;
    Console.WriteLine(f(a));
  end Main;
end Program;
