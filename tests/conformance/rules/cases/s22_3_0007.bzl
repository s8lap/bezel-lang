//! case: s22_3_0007
//! doc: spec
//! section: 22.3
//! title: writing move on a fresh producer is rejected as redundant
//! rule: "Writing [the move marker] on a borrow, a fresh producer, or a
//!   non-`owned` parameter position is likewise an error. A fresh producer —
//!   a `new` expression ... — stays unmarked: no live binding is
//!   invalidated, so there is nothing to announce."
//! cites: 22.3
//! expect: reject
//! diagnostic: fresh
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Registry
  private owned _items: OwningArray<Widget> := new OwningArray<Widget>();

  public constructor()
  end constructor;

  public method Append(owned item: Widget)
    _items.Add(move item);
  end Append;
end Registry;

public class Program
  public static method Main()
    using var r := new Registry();
    r.Append(move new Widget());
  end Main;
end Program;
