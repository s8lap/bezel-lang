//! case: s9_7_0035
//! doc: spec
//! section: 9.7
//! title: a record local carrying a member-stored using var may not be returned
//! rule: A record value held at a named location — a local or parameter, a field,
//!   a slot of an inline array held at one — is read with one source set ... a
//!   member store ... adds the sources of the borrows `e` carries to that set,
//!   never removing one, and is checked as an assignment of the updated value to
//!   the location that holds it ... A borrow-bearing record may be returned only when every borrow it
//!   carries has a source the caller re-establishes at the call site
//! cites: 9.7, 28.3
//! expect: reject
//! diagnostic: return
//! issue: 5734
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public record Box
  public property Held: Item?;

  public constructor()
    Held := null;
  end constructor;
end Box;

public class Maker
  public static method Make(): Box
    var p := new Box();
    using var it := new Item();
    p.Held := it;
    return p;
  end Make;
end Maker;
