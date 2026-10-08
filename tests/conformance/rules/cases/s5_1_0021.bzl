//! case: s5_1_0021
//! doc: spec
//! section: 5.1
//! title: a value type implicitly converts to Object by boxing
//! rule: Reference conversions are implicit: any type to Object — value types
//!   (primitives, records, enums, choices, non-owning inline arrays, the unit type) by
//!   boxing.
//! cites: 5.1, 8.6
//! expect: accept
module Conf;

public class Program
  public static method Convert(n: int)
    var o: Object := n;
  end Convert;
end Program;
