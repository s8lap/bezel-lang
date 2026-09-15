//! case: s28_11_0007
//! doc: spec
//! section: 28.11
//! title: the same owning collection over a class element type is admitted
//! rule: The owning-family column admits a **class** element type: the slot
//!   owns its element and the container's `Dispose()` cascades to it, which is
//!   the family's defining behaviour.
//! cites: 28.11
//! expect: accept
//! issue: 4393
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Size(): int
    using var widgets := new OwningArray<Widget>();
    widgets.Add(new Widget());
    return widgets.Count;
  end Size;
end Program;
