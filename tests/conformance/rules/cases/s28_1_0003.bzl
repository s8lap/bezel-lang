//! case: s28_1_0003
//! doc: spec
//! section: 28.1
//! title: owned on a return type of class type is admitted
//! rule: The `owned` modifier is admissible on return type declarations: `method
//!   Acquire(): owned Connection` transfers responsibility for disposal to the
//!   caller.
//! expect: accept
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Factory
  public method Acquire(): owned Connection
    return new Connection();
  end Acquire;
end Factory;
