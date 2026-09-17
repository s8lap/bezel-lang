//! case: s5_1_0018
//! doc: spec
//! section: 5.1
//! title: a non-nullable value implicitly converts to its nullable form
//! rule: For any type T, T implicitly converts to T? — for a value type, the
//!   present-value wrapping.
//! cites: 5.1
//! expect: accept
module Conf;

public class Program
  public static method Convert(n: int)
    var m: int? := n;
  end Convert;
end Program;
