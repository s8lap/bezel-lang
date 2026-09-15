//! case: s17_2_0012
//! doc: spec
//! section: 17.2
//! title: range mixed with bitwise AND, parenthesized, is admitted
//! rule: "write `(mask & 0)..n` or `mask & (0..n)` to disambiguate" — either
//!   parenthesization of a range-bitwise mix is admitted.
//! cites: 17.2
//! expect: accept
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method Make(mask: int, n: int)
    var r := (mask & 0)..n;
  end Make;
end Ops;
