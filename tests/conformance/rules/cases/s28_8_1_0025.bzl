//! case: s28_8_1_0025
//! doc: spec
//! section: 28.8.1
//! title: a parenthesized replacement that is not an assignment list is the replacing move's clause
//! rule: One tie-break decides every spelling: `with` followed by `(` and
//!   `Identifier :=` is the record-update suffix binding to the expression
//!   before it ... any other `with` opens the cancellation clause in an
//!   `await for` header and the replacement clause after a `move` operand
//! cites: 28.8.1, 9.5
//! expect: accept
//! issue: 5923
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Holder
  private owned _r: Res := new Res();

  public method Renew(): owned Res
    return move _r with (new Res());
  end Renew;
end Holder;
