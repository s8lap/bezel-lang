//! case: s9_7_0005
//! doc: spec
//! section: 9.7
//! title: a borrow-bearing record may not be stored in a non-null static field
//! rule: A borrow-bearing record may not be stored in a static field of any
//!   type, except when every reference-typed field of the value is null.
//! cites: 22.12
//! expect: reject
//! diagnostic: static
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle(Conn: Connection);

public class Registry
  private static owned readonly _conn: Connection := new Connection();
  public static readonly Default: Handle := new Handle(_conn);
end Registry;
