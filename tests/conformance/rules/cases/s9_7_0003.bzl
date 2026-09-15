//! case: s9_7_0003
//! doc: spec
//! section: 9.7
//! title: a plain reference-typed field in a record is a borrow, not an owned obligation
//! rule: Reference-typed fields in records are always borrows; the record does
//!   not own what they point to.
//! cites: 28.3
//! expect: accept
module Conf;

public class Connection
  public property Id: int;

  public constructor(id: int)
    Id := id;
  end constructor;
end Connection;

public record Handle(Conn: Connection);

public class Program
  public static method Read(): int
    using var c := new Connection(7);
    var h := new Handle(c);
    return h.Conn.Id;
  end Read;
end Program;
