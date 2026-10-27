//! case: s35_3_0036
//! doc: spec
//! section: 35.3
//! title: a generic choice's payload-free case written with an empty argument list is rejected
//! rule: For a generic choice the type arguments are written at the construction
//!   (Result<int, string>.Ok(value)) ... the same qualified-construction reading applies,
//!   and a payload-free case is a value expression, not a call.
//! cites: 14.5, 35.5
//! expect: reject
//! diagnostic: no payload
module Conf;

public choice Opt<T>
  Yes(v: T),
  No;
end Opt;

public class Program
  public static method Main()
    var o := Opt<int>.No();   // ERROR: `Opt<int>.No` is a value, not a call
  end Main;
end Program;
