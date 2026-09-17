//! case: s9_7_0007
//! doc: spec
//! section: 9.7
//! title: a borrow-bearing record may not be stored in an instance field of a class
//! rule: A borrow-bearing record value may not be stored in an instance field of
//!   a class; a class instance's lifetime is not bounded by any borrow source's
//!   scope, so the store is rejected statically.
//! expect: reject
//! diagnostic: instance field
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle(Conn: Connection);

public class Cache
  private _handle: Handle;

  public constructor(handle: Handle)
    _handle := handle;
  end constructor;
end Cache;
