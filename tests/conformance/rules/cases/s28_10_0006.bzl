//! case: s28_10_0006
//! doc: spec
//! section: 28.10
//! title: an explicit Dispose of a borrow-pin's source is a site-announced kill the pin leaves unfrozen
//! rule: The using var still pins the borrow's source for the duration of the
//!   scope ... the site-announced kills stay unfrozen under their flag
//!   backstop ... an explicit `Dispose()` of the source is permitted; a read
//!   through the pinned binding that the disposal dominates is §28.2's
//!   compile error, and one the disposal reaches on only some paths raises
//!   `DisposedAccessException` in checked disposal mode.
//! cites: 28.10, 28.3, 28.2
//! expect: accept
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
    h.Dispose();
  end Run;
end Program;
