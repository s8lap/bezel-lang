//! case: l2_5_0014
//! doc: stdlib
//! section: 2.5
//! title: DateTime(2024) is rejected: a one-argument DateTime construction matches no declared constructor
//! rule: Every parameter that declares no default must be bound, positionally or by name; a construction
//!   that binds no declared constructor of the type is a compile error (spec 22.5, 22.6). A one-argument DateTime construction matches no declared constructor.
//! cites: 2.5, 22.5, 22.6
//! expect: reject
//! diagnostic: no constructor
module Conf;

public class Program
  public static method Main()
    var x := new DateTime(2024);
  end Main;
end Program;
