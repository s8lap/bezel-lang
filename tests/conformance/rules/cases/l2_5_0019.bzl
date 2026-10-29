//! case: l2_5_0019
//! doc: stdlib
//! section: 2.5
//! title: DateOnly() is rejected: DateOnly declares no parameterless constructor
//! rule: Every parameter that declares no default must be bound, positionally or by name; a construction
//!   that binds no declared constructor of the type is a compile error (spec 22.5, 22.6). DateOnly declares no parameterless constructor.
//! cites: 2.5, 22.5, 22.6
//! expect: reject
//! diagnostic: no constructor
module Conf;

public class Program
  public static method Main()
    var x := new DateOnly();
  end Main;
end Program;
