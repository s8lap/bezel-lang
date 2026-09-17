//! case: s22_3_0005
//! doc: spec
//! section: 22.3
//! title: omitting the move marker on a named owned binding argument is rejected
//! rule: Omitting the marker on a named owned binding is a compile error
//!   naming the fix ("this call consumes `x`; write `M(move x)`").
//! cites: 22.3
//! expect: reject
//! diagnostic: consumes
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
    var w := new Widget();
    r.Append(w);
  end Main;
end Program;
