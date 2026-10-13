//! case: s28_11_0012
//! doc: spec
//! section: 28.11
//! title: a RemoveAt at the borrow's index after an intervening Insert draws no warning
//! rule: A borrow so produced is valid until its slot is mutated — an
//!   indexer-set, `Remove`/`RemoveAt`, `Clear()` or the collection's own
//!   disposal disposes the referent — and is not statically tracked per slot:
//!   a use of it afterwards is a mid-scope kill under the checked-mode
//!   backstop (K4.1, §28.2)
//! cites: 28.11, 29.12, 28.2
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
    var w := xs[0];
    xs.Insert(0, new Widget());
    xs.RemoveAt(0);
    w.Ping();
  end Demo;
end Program;
