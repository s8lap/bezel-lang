//! case: s28_1_0001
//! doc: spec
//! section: 28.1
//! title: owned on a method parameter of class type is admitted
//! rule: The `owned` modifier is admissible on method parameter declarations: `method
//!   M(owned p: T)` takes ownership of the value at the call site.
//! cites: C.1
//! expect: accept
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Pool
  public method Adopt(owned c: Connection)
    c.Dispose();
  end Adopt;
end Pool;
