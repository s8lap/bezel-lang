//! case: s28_6_0019
//! doc: spec
//! section: 28.6
//! title: using a case pattern binding without disposing it is admitted
//! rule: A pattern binding is a borrow of the scrutinee; ordinary borrow use
//!   (without disposal) is admitted.
//! cites: 19.3, 28.6
//! expect: accept
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
      var w: Widget => Console.WriteLine("found")
    end case;
  end Inspect;
end Holder;
