//! case: l2_5_0026
//! doc: stdlib
//! section: 2.5
//! title: TimeOnly(1, millisecond := 5) is rejected: the required minute is left unbound
//! rule: Every parameter that declares no default must be bound, positionally or by name; a construction
//!   that binds no declared constructor of the type is a compile error (spec 22.5, 22.6). Naming only millisecond after hour leaves the required minute unbound.
//! cites: 2.5, 22.5, 22.6
//! expect: reject
//! diagnostic: no constructor
module Conf;

public class Program
  public static method Main()
    var x := new TimeOnly(1, millisecond := 5);
  end Main;
end Program;
