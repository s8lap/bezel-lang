//! case: s22_3_0012
//! doc: spec
//! section: 22.3
//! title: a ref argument's static type must exactly match the referent type
//! rule: "**Exact referent type.** The argument's static type must be
//!   **exactly** the parameter's referent type. Because the parameter
//!   aliases the caller's location, no implicit conversion is admitted —
//!   not even a widening one that an ordinary by-value argument would take."
//! cites: 22.3
//! expect: reject
//! diagnostic: exact
module Conf;

public class Program
  public static method Bump(ref n: long)
    n := n + 1L;
  end Bump;

  public static method Main()
    var x: int := 1;
    Bump(ref x);
  end Main;
end Program;
