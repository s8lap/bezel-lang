//! case: s9_7_0034
//! doc: spec
//! section: 9.7
//! title: a member store of an inner-block using var into an outer record local is rejected
//! rule: A record value held at a named location — a local or parameter, a field,
//!   a slot of an inline array held at one — is read with one source set ... a
//!   member store ... adds the sources of the borrows `e` carries to that set,
//!   never removing one, and is checked as an assignment of the updated value to
//!   the location that holds it ... at a local or parameter by §28.3's local-scope bullet
//! cites: 9.7, 28.3
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
    var p := new Box();
    do
      using var it := new Item();
      p.Held := it;
    end;
    return 0;
  end Run;
end Program;
