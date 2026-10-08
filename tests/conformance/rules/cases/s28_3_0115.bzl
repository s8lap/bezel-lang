//! case: s28_3_0115
//! doc: spec
//! section: 28.3
//! title: an out location filled by a constructor call bound to a using var lives with that binding
//! rule: The caller therefore treats each such location as receiving any borrow
//!   the call carries: the sources the receiver and every plain (non-`owned`,
//!   non-`scoped`) argument contribute ... (the receiver as an argument of
//!   its type: at a constructor call, the object or value constructed, read
//!   as the binding or field its result initializes or, for an unbound
//!   result, as an unbound owned temporary bounded to the full expression
//! cites: 28.3, 22.3
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public class Holder
  private owned _r: Item;

  public constructor(out o: Item)
    _r := new Item();
    o := _r;
  end constructor;
end Holder;

public class Client
  public static method Run(): int
    using var h := new Holder(out var o);
    return o.Size();
  end Run;
end Client;
