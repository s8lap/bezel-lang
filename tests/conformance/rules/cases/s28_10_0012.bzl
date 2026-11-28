//! case: s28_10_0012
//! doc: spec
//! section: 28.10
//! title: a using-var pin over a borrow-returning call is rejected after the source's dominating Dispose
//! rule: or a local bound from a member read or call through the variable or through such a holder
//!   (`var v := a.F;`, `var v := a.Get();`, a borrowing-match binding over `a.F`) … a use of the holder —
//!   a read or member access through it … — is the same compile error
//! cites: 28.2, 28.10, 28.3
//! expect: reject
//! diagnostic: dominat
//! flags: --checked-disposal
//! issue: 4413
//! notes: v0.361 (#8708) flips this case from run/faulted to reject: `view` is bound from the call
//!   `h.Borrowed()` and `h.Dispose()` dominates its read. The kill stays permitted; only the use is
//!   rejected. The checked-mode backstop is now reached only by a kill on some paths.
module Conf;

public class Widget
  public property Label: string;

  public constructor()
    Label := "live";
  end constructor;

  destructor
  end destructor;
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
