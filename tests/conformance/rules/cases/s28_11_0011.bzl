//! case: s28_11_0011
//! doc: spec
//! section: 28.11
//! title: an element-level indexer write on the collection does not draw the element-borrow warning
//! rule: A borrow so produced is valid until its slot is mutated — an
//!   indexer-set, `Remove`/`RemoveAt`, `Clear()` or the collection's own
//!   disposal disposes the referent — and is not statically tracked per slot
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
