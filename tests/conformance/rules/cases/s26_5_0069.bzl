//! case: s26_5_0069
//! doc: spec
//! section: 26.5
//! title: a bound reference is refused when the extension block extends a sibling collection type
//! rule: Where neither rule 1 nor this search supplies the name, nothing
//!   supplies it: the access names no member and is a compile error at the
//!   name, the diagnostic naming the receiver's static type
//! cites: 26.5, 27.6
//! expect: reject
//! diagnostic: no member
//! issue: 7488
module Conf;

extension Queue<int>
  public method Length(): int => 42;
end;

public class Program
  public static method Main()
    using var s := new Stack<int>();
    s.Push(1);
    var f: () -> int := s.Length;
    Console.WriteLine(f());
  end Main;
end Program;
