//! case: s28_5_0001
//! doc: spec
//! section: 28.5
//! title: an owned field of class type is admitted and auto-disposed with its container
//! rule: Owned fields are restricted to class- or interface-typed fields; a class
//!   holding one needs no destructor, the field being disposed with its container.
//! cites: 28.1, 28.5, C.3
//! expect: accept
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Pool
  private owned _c: Connection;

  public constructor()
    _c := new Connection();
  end constructor;
end Pool;
