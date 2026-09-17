//! case: s28_3_0010
//! doc: spec
//! section: 28.3
//! title: move on a return operand under an owned return is rejected as redundant
//! rule: The transfer marker is rejected as redundant at a marker-free position:
//!   `return x;` under an owned return, never `return move x;`.
//! expect: reject
//! diagnostic: return
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Factory
  public method Acquire(): owned Widget
    var w := new Widget();
    return move w;
  end Acquire;
end Factory;
