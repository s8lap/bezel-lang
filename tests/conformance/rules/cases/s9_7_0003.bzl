//! case: s9_7_0003
//! doc: spec
//! section: 9.7
//! title: a plain reference-typed field in a record is a borrow, not an owned obligation
//! rule: Reference-typed fields in records are always borrows; the record does
//!   not own what they point to.
//! expect: accept
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
