//! case: s9_7_0011
//! doc: spec
//! section: 9.7
//! title: a borrow-bearing record carrying a borrow of a local may not be returned
//! rule: A borrow-bearing record may not be returned from a method whose
//!   return-type rules cannot establish a sufficient lifetime for every borrow
//!   the record carries.
//! cites: 28.3
//! expect: reject
//! diagnostic: lifetime
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle(Conn: Connection?);

public class Program
  public static method Make(): Handle
    var c := new Connection();
    return new Handle(c);
  end Make;
end Program;
