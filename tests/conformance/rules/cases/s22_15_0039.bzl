//! case: s22_15_0039
//! doc: spec
//! section: 22.15
//! title: a pure body may not construct a built-in exception outside a throw operand
//! rule: Inside a pure body, `new E(args)` is admitted only as the immediate
//!   operand of a `throw` statement or `throw` expression, and only when
//! cites: 22.15
//! expect: reject
//! diagnostic: class construction
//! issue: 7453
module Conf;

public class Ops
  public static pure method Make(): string
    var e := new FormatException("x");
    return "y";
  end Make;
end Ops;
