//! case: s28_3_0019
//! doc: spec
//! section: 28.3
//! title: an owning iterator's yield return carries no marker
//! rule: The owning iterator's declared return type is the announcement; `yield return
//!   x;` is marker-free.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Factory
  public method Produce(): IOwningEnumerable<Widget>
    var w := new Widget();
    yield return w;
  end Produce;
end Factory;
