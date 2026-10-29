//! case: l2_5_0018
//! doc: stdlib
//! section: 2.5
//! title: DateTime with nine arguments is rejected: it exceeds the longest declared constructor
//! rule: Every parameter that declares no default must be bound, positionally or by name; a construction
//!   that binds no declared constructor of the type is a compile error (spec 22.5, 22.6). A nine-argument DateTime construction exceeds the longest declared constructor.
//! cites: 2.5, 22.5, 22.6
//! expect: reject
//! diagnostic: no constructor
module Conf;

public class Program
  public static method Main()
    var x := new DateTime(2024, 3, 4, 5, 6, 7, 8, 9, 10);
  end Main;
end Program;
