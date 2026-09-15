//! case: s28_3_0030
//! doc: spec
//! section: 28.3
//! title: returning a receiver-sourced borrow is admitted
//! rule: A receiver-sourced borrow from an instance method — a bare field read — is an
//!   admissible borrow return.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  private _w: Widget;

  public constructor(w: Widget)
    _w := w;
  end constructor;

  public method Peek(): Widget
    return _w;
  end Peek;
end Program;
