//! case: s28_6_0018
//! doc: spec
//! section: 28.6
//! title: disposing a case pattern binding is rejected
//! rule: The borrow receivers for `Dispose()` include a pattern binding.
//! cites: 19.3, 28.6
//! expect: reject
//! diagnostic: pattern
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private _w: Widget;

  public constructor(w: Widget)
    _w := w;
  end constructor;

  public method Current(): Widget => _w;

  public method Inspect()
    case Current() of
      var w: Widget => w.Dispose()
    end case;
  end Inspect;
end Holder;
