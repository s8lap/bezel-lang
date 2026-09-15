//! case: s28_3_0018
//! doc: spec
//! section: 28.3
//! title: move on an owning iterator's yield return is rejected as redundant
//! rule: The transfer marker is rejected as redundant on an owning iterator's `yield
//!   return`: the declared owning interface already announces.
//! expect: reject
//! diagnostic: yield
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Factory
  public method Produce(): IOwningEnumerable<Widget>
    var w := new Widget();
    yield return move w;
  end Produce;
end Factory;
