//! case: s28_11_0011
//! doc: spec
//! section: 28.11
//! title: an element-level indexer write on the collection does not draw the element-borrow warning
//! rule: An element-level write — an indexer-set — does not draw this
//!   warning: it is not a §29.12 structural operation, and the two warnings
//!   share one operation vocabulary.
//! cites: 28.11, 29.12
//! expect: quiet
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
  end Ping;

  destructor
  end destructor;
end Widget;

public class Program
  public method Demo()
    using var xs := new OwningArray<Widget>();
    xs.Add(new Widget());
    xs.Add(new Widget());
    var w := xs[0];
    xs[1] := new Widget();
    w.Ping();
  end Demo;
end Program;
