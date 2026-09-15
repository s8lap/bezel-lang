//! case: s9_7_0009
//! doc: spec
//! section: 9.7
//! title: borrow-carrying is transitive through another borrow-bearing record field
//! rule: A field is borrow-carrying when its type is another borrow-bearing
//!   record, or a borrow-bearing choice (transitivity); such a record is itself
//!   borrow-bearing and may not be stored in an instance field of a class.
//! cites: 3.6
//! expect: reject
//! diagnostic: instance field
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle(Conn: Connection);

public record Session(At: Handle, Id: int);

public class Cache
  private _session: Session;

  public constructor(session: Session)
    _session := session;
  end constructor;
end Cache;
