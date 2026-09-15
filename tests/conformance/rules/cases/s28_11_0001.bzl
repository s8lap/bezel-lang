//! case: s28_11_0001
//! doc: spec
//! section: 28.11
//! title: storing a parameter-sourced borrow into a field-held borrowing collection is admitted
//! rule: Storing a class-typed borrow into a borrowing collection follows
//!   the same shape as §28.3's borrowed-field rule — parameter-sourced and
//!   receiver-sourced borrows are admitted at the store site.
//! cites: 28.11, 28.3
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Registry
  private _items: Array<Widget>;

  public constructor()
    _items := new Array<Widget>();
  end constructor;

  public method Track(w: Widget)
    _items.Add(w);
  end Track;
end Registry;
