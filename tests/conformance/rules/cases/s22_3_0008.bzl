//! case: s22_3_0008
//! doc: spec
//! section: 22.3
//! title: a fresh producer stays unmarked at an owned parameter
//! rule: A fresh producer — a `new` expression, a call returning `owned T`,
//!   an operator result, or a `move obj._field` — stays unmarked: no live
//!   binding is invalidated, so there is nothing to announce.
//! cites: 22.3
//! expect: accept
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

public class Runner
  public static method Run()
    using var r := new Registry();
    r.Append(new Widget());
  end Run;
end Runner;
