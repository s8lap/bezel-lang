//! case: l2_5_0032
//! doc: stdlib
//! section: 2.5
//! title: DateOnly with named year and month but no day is rejected
//! rule: Every parameter that declares no default must be bound, positionally or by name (spec 22.5);
//!   `DateOnly(year, month, day)` has no default for `day`.
//! cites: 2.5, 22.5
//! expect: reject
//! diagnostic: not supplied
module Conf;

public class Program
  public static method Main()
    var d := new DateOnly(year := 2024, month := 5);
  end Main;
end Program;
