//! case: l6_5_0021
//! doc: stdlib
//! section: 6.5
//! title: an EventSource over a copyable choice is admitted
//! rule: `EventSource<T>` and `AsyncEventSource<T>` admit no move-only value
//!   type (language spec §15.3 — an owning inline array or an owning choice)
//!   as `T`
//! cites: 6.5
//! expect: accept
module Conf;

public class Conn
  public constructor()
  end constructor;
end Conn;

public choice Slot
  Full(c: int),
  Empty;
end Slot;

public class Feed
  private owned readonly _src: EventSource<Slot> := new();

  public constructor()
  end constructor;
end Feed;
