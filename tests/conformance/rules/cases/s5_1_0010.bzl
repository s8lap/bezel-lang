//! case: s5_1_0010
//! doc: spec
//! section: 5.1
//! title: decimal converts from double only through an explicit conversion
//! rule: decimal and binary floats are never implicit (§5.1); decimal(x) for
//!   binary-float x yields the value of x's shortest round-trip form under
//!   an explicit conversion (§5.2). Twin of s5_1_0009: the same pair, spelled
//!   through the admitted explicit form.
//! cites: 5.1, 5.2
//! expect: accept
module Conf;

public class Program
  public static method Convert(d: double)
    var m: decimal := decimal(d);
  end Convert;
end Program;
