//! case: s28_10_0009
//! doc: spec
//! section: 28.10
//! title: moving a borrow-pin's source is a frozen kill the pin still rejects
//! rule: The using var still pins the borrow's source for the duration of the
//!   scope — the borrow stays live until the using-var's scope ends, and the
//!   consume-site freeze then governs every consume of the source in that
//!   range: the frozen kills — a move, a transfer, a reassignment of the
//!   binding — are compile errors.
//! cites: 28.10, 28.3
//! expect: reject
//! diagnostic: pin
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private owned _w: Widget;

  public constructor()
    _w := new Widget();
  end constructor;

  public method Borrowed(): Widget => _w;
end Holder;

public class Program
  public method Run()
    var h := new Holder();
    using var view := h.Borrowed();
    var taken := move h;
    taken.Dispose();
  end Run;
end Program;
