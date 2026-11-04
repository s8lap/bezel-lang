//! case: s26_5_0073
//! doc: spec
//! section: 26.5
//! title: a type-qualified reference to a name no extension supplies is refused
//! rule: Where neither rule 1 nor this search supplies the name, nothing
//!   supplies it: the access names no member and is a compile error at the
//!   name, the diagnostic naming the receiver's static type
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
    var f: (Array<int>) -> int := Array<int>.Bogus;
  end Main;
end Program;
