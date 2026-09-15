//! case: s9_7_0005
//! doc: spec
//! section: 9.7
//! title: a borrow-bearing record may not be stored in a non-null static field
//! rule: A borrow-bearing record may not be stored in a static field of any
//!   type, except when every reference-typed field of the value is null.
//! expect: reject
//! diagnostic: static
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle
  public readonly property Conn: Connection;

  public constructor(conn: Connection)
    Conn := conn;
  end constructor;
end Handle;

public class Registry
  public static readonly Default: Handle;

  static constructor()
    using var c := new Connection();
    Default := new Handle(c);
  end constructor;
end Registry;
