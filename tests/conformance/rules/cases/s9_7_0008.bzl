//! case: s9_7_0008
//! doc: spec
//! section: 9.7
//! title: a borrow-bearing record in the null-source form may be stored in a static field
//! rule: A borrow-bearing record may not be stored in a static field of any
//!   type, except when every reference-typed field (and every reference-typed
//!   slot of every inline-array field) of the value is null — the null-source
//!   form.
//! cites: 22.12
//! expect: accept
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle(Conn: Connection?);

public class Registry
  public static readonly None: Handle := new Handle(null);
end Registry;
