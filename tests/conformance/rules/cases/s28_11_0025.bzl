//! case: s28_11_0025
//! doc: spec
//! section: 28.11
//! title: an iterator declared to return IEnumerable over an owning choice is rejected
//! rule: `IEnumerable<T>`, `IEnumerator<T>`, `IAsyncEnumerable<T>` and
//!   `IAsyncEnumerator<T>` (stdlib §4.12) and `EventSource<T>` and
//!   `AsyncEventSource<T>` (stdlib §6.5) admit no move-only value type (§15.3)
//!   as `T` ... Every naming is a site (§14.1 *Instantiation and its site*):
//!   an iterator's or async iterator's declared return type
//! cites: 28.11, 15.3, 29.3
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

public class Source
  public static method Each(): IEnumerable<Slot>
    yield return Slot.Empty;
  end Each;
end Source;
