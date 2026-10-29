//! case: l2_5_0029
//! doc: stdlib
//! section: 2.5
//! title: DateTime(year := 1, month := 2, hour := 3) is rejected: it binds no declared constructor
//! rule: Every parameter that declares no default must be bound, positionally or by name; a construction
//!   that binds no declared constructor of the type is a compile error (spec 22.5, 22.6). Naming year, month and hour binds no declared DateTime constructor (day, minute and second are unbound).
//! cites: 2.5, 22.5, 22.6
//! expect: reject
//! diagnostic: no constructor
module Conf;

public class Program
  public static method Main()
    var x := new DateTime(year := 1, month := 2, hour := 3);
  end Main;
end Program;
