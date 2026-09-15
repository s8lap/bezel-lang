//! case: s9_7_0001
//! doc: spec
//! section: 9.7
//! title: a record cannot declare an owned field
//! rule: Records cannot have destructors, owned fields, or implement
//!   IDisposable.
//! expect: reject
//! diagnostic: owned
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle
  private owned _conn: Connection;

  public constructor(conn: Connection)
    _conn := conn;
  end constructor;
end Handle;
