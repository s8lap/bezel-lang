//! case: s28_13_0005
//! doc: spec
//! section: 28.13
//! title: Get()'s borrow does not outlive the handle that sourced it
//! rule: `Get(): T` produces a borrow of the underlying value whose source
//!   is the handle, valid while that handle lives. The borrow does not
//!   extend the value's lifetime; the handle does.
//! cites: 28.13, 28.3
//! expect: reject
//! diagnostic: borrow
module Conf;

public class Connection
  public constructor()
  end constructor;

  public method Tag(): string => "conn";
end Connection;

public class Leaker
  public constructor()
  end constructor;

  public method Peek(): Connection
    using var h := new Shared<Connection>(new Connection());
    return h.Get();
  end Peek;
end Leaker;
