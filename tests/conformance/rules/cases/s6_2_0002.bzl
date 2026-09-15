//! case: s6_2_0002
//! doc: spec
//! section: 6.2
//! title: arithmetic between double and decimal has no implicit conversion and is a compile error
//! rule: If no implicit conversion exists between the operand types (e.g.
//!   double and decimal), the operation is a compile error and an explicit
//!   cast is required.
//! cites: 6.2
//! expect: reject
//! diagnostic: explicit cast is required
module Conf;

public class Program
  public static method Make(d: double, m: decimal)
    var r := d + m;
  end Make;
end Program;
