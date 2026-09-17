//! case: s22_3_0006
//! doc: spec
//! section: 22.3
//! title: a named owned binding fed with the move marker reaches the owned parameter
//! rule: "**Call-site marker:** when the argument is a named owned binding,
//!   the caller writes the transfer marker at the call site: `M(move x)`."
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
    var w := new Widget();
    r.Append(move w);
  end Run;
end Runner;
