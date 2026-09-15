//! case: s5_3_0006
//! doc: spec
//! section: 5.3
//! title: a nullable cast whose non-nullable conversion is inadmissible is rejected
//! rule: For a target type T, the nullable cast form T?(x) is admitted exactly
//!   when the corresponding non-nullable conversion T(v) is admitted from the
//!   non-nullable form of x's type — and bool(n) remains an error in both the
//!   implicit and explicit forms (§5.2).
//! cites: 5.3, 5.2
//! expect: reject
//! diagnostic: remains an error
module Conf;

public class Program
  public static method Main()
    var n: int? := 1;
    var b := bool?(n);
  end Main;
end Program;
