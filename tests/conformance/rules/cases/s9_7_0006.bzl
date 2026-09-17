//! case: s9_7_0006
//! doc: spec
//! section: 9.7
//! title: a borrow-bearing record may not be constructed from a temporary
//! rule: A borrow-bearing record may not be constructed from a temporary bound
//!   to one of its borrow-carrying fields; a construction argument that is a
//!   fresh call result not bound to a named location has a lifetime that ends at
//!   the construction expression.
//! expect: reject
//! diagnostic: temporary
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle(Conn: Connection);

public class Program
  public static method Touch()
    var h := new Handle(new Connection());
  end Touch;
end Program;
