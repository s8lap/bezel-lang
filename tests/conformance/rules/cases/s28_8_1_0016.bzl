//! case: s28_8_1_0016
//! doc: spec
//! section: 28.8.1
//! title: a new expression's argument expressions classify on their own between the move and the restoration
//! rule: The exclusion covers the construction only — a `new` expression's
//!   argument expressions classify on their own, so `_r := new Res(Fetch());`
//!   still puts Fetch()'s exception edge between the move and the
//!   restoration.
//! cites: 28.8.1
//! expect: reject
//! diagnostic: not reassigned
module Conf;

public class Widget
  public constructor(tag: int)
  end constructor;
end Widget;

public class Supplier
  public static method Fetch(): int
    return 1;
  end Fetch;
end Supplier;

public class Holder
  private owned _resource: Widget;

  public constructor()
    _resource := new Widget(0);
  end constructor;

  public method Wrong(): owned Widget
    var old := move _resource;
    _resource := new Widget(Supplier.Fetch());
    return old;
  end Wrong;
end Holder;
