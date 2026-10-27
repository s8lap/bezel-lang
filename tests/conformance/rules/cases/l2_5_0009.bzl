//! case: l2_5_0009
//! doc: stdlib
//! section: 2.5
//! title: DateTime.Parse reads the round-trip form and renders it back
//! rule: stdlib §2.5's parse contract: "`DateTime.Parse` accepts exactly the
//!     type's own rendered forms — the round-trip form `yyyy-MM-ddTHH:mm:ss`,
//!     optionally extended with a fractional-seconds tail `.fff`, and the
//!     date-only form `yyyy-MM-dd` (time components zero)". The no-argument
//!     `ToString()` renders the round-trippable ISO 8601 form
//!     `yyyy-MM-ddTHH:mm:ss`.
//! cites: 2.5
//! expect: run
//! stdout: 2020-01-02T03:04:05
module Conf;

public class Program
  public static method Main()
    var d := DateTime.Parse("2020-01-02T03:04:05");
    Console.WriteLine(d.ToString());
  end Main;
end Program;
