//! case: s14_4_0002
//! doc: spec
//! section: 14.4
//! title: OwningArray<T> is the sanctioned replacement for Array<owned T>
//! rule: The owning forms are separate named types with their own
//!   declarations: `OwningArray<T>` beside `Array<T>` (§14.4).
//! cites: 14.4
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private owned _items: OwningArray<Widget>;

  public constructor()
    _items := new();
  end constructor;
end Holder;
