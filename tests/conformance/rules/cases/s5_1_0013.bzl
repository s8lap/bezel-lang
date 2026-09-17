//! case: s5_1_0013
//! doc: spec
//! section: 5.1
//! title: comparing an int to zero is the legal spelling the rejected int-to-bool conversion needs
//! rule: Between integers and bool, conversion is never implicit (§5.1); a
//!   comparison such as n <> 0 produces bool directly. Twin of s5_1_0012: same
//!   source, the comparison in place of the rejected conversion.
//! cites: 5.1
//! expect: accept
module Conf;

public class Program
  public static method Convert(n: int)
    var b: bool := n <> 0;
  end Convert;
end Program;
