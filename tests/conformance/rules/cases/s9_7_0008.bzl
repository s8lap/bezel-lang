//! case: s9_7_0008
//! doc: spec
//! section: 9.7
//! title: a borrow-bearing record in the null-source form may be stored in a static field
//! rule: A borrow-bearing record may not be stored in a static field of any
//!   type, except when every reference-typed field of the value is null (the
//!   null-source form).
//! cites: C.3
//! expect: accept
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle
  public readonly property Conn: Connection?;

  public constructor(conn: Connection?)
    Conn := conn;
  end constructor;
end Handle;

public class Registry
  public static readonly None: Handle := new Handle(null);
end Registry;
