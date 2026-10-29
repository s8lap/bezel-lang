//! case: l2_5_0025
//! doc: stdlib
//! section: 2.5
//! title: TimeOnly with five arguments is rejected: it exceeds the declared constructor
//! rule: Every parameter that declares no default must be bound, positionally or by name; a construction
//!   that binds no declared constructor of the type is a compile error (spec 22.5, 22.6). A five-argument TimeOnly construction exceeds the declared constructor.
//! cites: 2.5, 22.5, 22.6
//! expect: reject
//! diagnostic: no constructor
module Conf;

public class Program
  public static method Main()
    var x := new TimeOnly(1, 2, 3, 4, 5);
  end Main;
end Program;
