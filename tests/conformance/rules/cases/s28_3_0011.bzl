//! case: s28_3_0011
//! doc: spec
//! section: 28.3
//! title: a return operand under an owned return carries no marker
//! rule: `return` under an `owned` return type is marker-free: the declared return type
//!   already announces the transfer.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Factory
  public method Acquire(): owned Widget
    var w := new Widget();
    return w;
  end Acquire;
end Factory;
