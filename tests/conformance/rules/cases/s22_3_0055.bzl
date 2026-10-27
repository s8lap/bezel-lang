//! case: s22_3_0055
//! doc: spec
//! section: 22.3
//! title: a fresh producer is admitted at an owned parameter reached through a constraint head
//! rule: A **fresh producer** — a `new` expression, a call returning `owned T`, an
//!   operator result, or a `move obj._field` — stays unmarked: no live
//!   binding is invalidated, so there is nothing to announce.
//!   Inside the generic body, T's surface includes IFoo's members, regardless
//!   of which conformance category T belongs to (§14.2), so the call is an
//!   ordinary call whose argument forms are read at the head's own
//!   signature (stdlib §4.2 declares `Add(owned item: T)`).
//! cites: 22.3, 28.8, 14.2
//! expect: accept
//! issue: 7086
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public static class Helpers
  public static method Fill<T>(target: T): int where T: IOwningCollection<Item>
    target.Add(new Item());
    return 1;
  end Fill;
end Helpers;
