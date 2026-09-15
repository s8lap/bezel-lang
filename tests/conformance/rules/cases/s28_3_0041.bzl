//! case: s28_3_0041
//! doc: spec
//! section: 28.3
//! title: a property getter returning a fresh owned value is rejected
//! rule: A property or indexer getter may never return a fresh owned value; getters
//!   lend only.
//! expect: reject
//! diagnostic: getter
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Factory
  public property Fresh: Widget => new Widget();
end Factory;
