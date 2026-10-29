//! case: l2_5_0028
//! doc: stdlib
//! section: 2.5
//! title: TimeOnly(minute := 1, second := 2) is rejected: the required hour is left unbound
//! rule: Every parameter that declares no default must be bound, positionally or by name; a construction
//!   that binds no declared constructor of the type is a compile error (spec 22.5, 22.6). Naming minute and second leaves the required hour unbound.
//! cites: 2.5, 22.5, 22.6
//! expect: reject
//! diagnostic: no constructor
module Conf;

public class Program
  public static method Main()
    var x := new TimeOnly(minute := 1, second := 2);
  end Main;
end Program;
