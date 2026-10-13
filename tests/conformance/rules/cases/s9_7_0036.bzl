//! case: s9_7_0036
//! doc: spec
//! section: 9.7
//! title: a member store at depth through an outer inline array of records from an inner block is rejected
//! rule: A record value held at a named location — a local or parameter, a field,
//!   a slot of an inline array held at one — is read with one source set ... a
//!   member store ... adds the sources of the borrows `e` carries to that set,
//!   never removing one, and is checked as an assignment of the updated value to
//!   the location that holds it ... and at an inline-array slot as §15.1.2's slot store into that array
//!   (`a[0].Item := e` over a local `[2]Box` adds to `a`'s one set)
//! cites: 9.7, 15.1.2, 28.3
//! expect: reject
//! diagnostic: outlive
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

public class Program
  public static method Run(): int
    var a: [2]Box := [new Box(), new Box()];
    do
      using var it := new Item();
      a[0].Held := it;
    end;
    return 0;
  end Run;
end Program;
