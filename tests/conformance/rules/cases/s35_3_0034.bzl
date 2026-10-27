//! case: s35_3_0034
//! doc: spec
//! section: 35.3
//! title: a nested choice's payload-free case written with an empty argument list is rejected
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call). The qualification of a nested choice reads
//!   through the enclosing declaration, and the case value it names takes no argument list.
//! cites: 35.5
//! expect: reject
//! diagnostic: no payload
module Conf;

public class Outer
  public choice Inner
    A(x: int),
    Nothing;
  end Inner;
end Outer;

public class Program
  public static method Main()
    var o := Outer.Inner.Nothing();   // ERROR: `Outer.Inner.Nothing` is a value, not a call
  end Main;
end Program;
