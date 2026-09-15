//! case: s17_6_0005
//! doc: spec
//! section: 17.6
//! title: a nullable operand to ++ is rejected
//! rule: "An operand whose static type is a nullable type is a compile
//!   error: the desugared `x + 1` has no meaning on a nullable operand
//!   (§7.11) ... write `x := (x ?? 0) + 1;`, `x := x! + 1;`, or narrow
//!   first."
//! cites: 17.6, 7.11
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Bump(n: int?)
    n++;
  end Bump;
end Program;
