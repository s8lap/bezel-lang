//! case: s28_10_0012
//! doc: spec
//! section: 28.10
//! title: a read through a borrow-pin after the source's explicit Dispose faults in checked mode
//! rule: The site-announced kills stay unfrozen under their flag backstop: an
//!   explicit `Dispose()` of a borrow-pin's source is permitted, and a read
//!   through the pinned binding afterwards raises `DisposedAccessException` in
//!   checked disposal mode.
//! cites: 28.10, 28.2, 28.3
//! expect: run
//! stdout: faulted
//! flags: --checked-disposal
//! issue: 4413
module Conf;

public class Widget
  public property Label: string;

  public constructor()
    Label := "live";
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
  public static method Main()
    var h := new Holder();
    using var view := h.Borrowed();
    h.Dispose();
    try
      Console.WriteLine(view.Label);
    catch
      var ex: DisposedAccessException => Console.WriteLine("faulted");
    end try;
  end Main;
end Program;
