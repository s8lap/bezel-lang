//! case: s28_1_0005
//! doc: spec
//! section: 28.1
//! title: owned on an out parameter of class type is admitted
//! rule: The `owned` modifier is admissible on `out` parameter declarations: `out owned
//!   p: T` produces an owned reference for the caller to take.
//! expect: accept
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Factory
  public method TryAcquire(out owned c: Connection)
    c := new Connection();
  end TryAcquire;
end Factory;
