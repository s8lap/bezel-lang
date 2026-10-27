//! case: l2_18_0015
//! doc: stdlib
//! section: 2.18
//! title: a format spec on a non-conforming operand is a compile error
//! rule: stdlib §2.18: "The primitive and temporal types are **intrinsically
//!     formattable** (they behave as conforming implementations without
//!     declaring the interface); a format spec on any other non-conforming
//!     operand is a compile error." The plain class below conforms to nothing
//!     that takes a format, so the hole's spec is a compile error.
//! cites: 2.18, 18.5
//! expect: reject
//! diagnostic: format
module Conf;

public class Plain
  public method ToString(): string
    return "plain";
  end ToString;
end Plain;

public class Program
  public static method Main()
    var p := new Plain();
    Console.WriteLine($"{p:X}");
  end Main;
end Program;
