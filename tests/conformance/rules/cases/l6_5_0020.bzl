//! case: l6_5_0020
//! doc: stdlib
//! section: 6.5
//! title: an EventSource over an owning choice is rejected at its naming
//! rule: **A move-only payload is not admitted at the source classes.**
//!   `EventSource<T>` and `AsyncEventSource<T>` admit no move-only value type
//!   (language spec §15.3 — an owning inline array or an owning choice) as
//!   `T`: a compile error at the instantiation site, through any chain of user
//!   declarations
//! cites: 6.5
//! expect: reject
//! diagnostic: move-only
//! issue: 5819
module Conf;

public class Conn
  public constructor()
  end constructor;
end Conn;

public choice Slot
  Full(owned c: Conn),
  Empty;
end Slot;

public class Feed
  private owned readonly _src: EventSource<Slot> := new();

  public constructor()
  end constructor;
end Feed;
