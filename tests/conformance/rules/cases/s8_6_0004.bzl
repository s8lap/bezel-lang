//! case: s8_6_0004
//! doc: spec
//! section: 8.6
//! title: a boxed borrow-bearing record may not be stored in a field
//! rule: The compiler enforces: a boxed borrow-bearing record value may not be
//!   stored in a location that could outlive the source of any borrow the record
//!   contains. The boxed copy on the heap holds the same borrowed references as
//!   the source record; it does not extend their lifetimes.
//! cites: 9.7, 28.3
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle(Conn: Connection);

public class Holder
  private _boxed: Object?;

  public constructor(h: Handle)
    _boxed := h;
  end constructor;
end Holder;
