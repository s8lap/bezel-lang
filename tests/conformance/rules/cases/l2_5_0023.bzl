//! case: l2_5_0023
//! doc: stdlib
//! section: 2.5
//! title: TimeOnly() is rejected: TimeOnly declares no parameterless constructor
//! rule: Every parameter that declares no default must be bound, positionally or by name; a construction
//!   that binds no declared constructor of the type is a compile error (spec 22.5, 22.6). TimeOnly declares no parameterless constructor.
//! cites: 2.5, 22.5, 22.6
//! expect: reject
//! diagnostic: no constructor
module Conf;

public class Program
  public static method Main()
    var x := new TimeOnly();
  end Main;
end Program;
