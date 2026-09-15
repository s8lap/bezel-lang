//! case: s28_11_0004
//! doc: spec
//! section: 28.11
//! title: Array<T> and OwningArray<T> are distinct, unrelated types with no implicit conversion
//! rule: There is no implicit conversion between the two families; an
//!   OwningArray<Connection> is not an Array<Connection> and vice versa.
//! cites: 28.11
//! expect: reject
//! diagnostic: convert
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Wrong(source: OwningArray<Widget>)
    var view: Array<Widget> := source;
  end Wrong;
end Program;
