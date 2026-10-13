//! case: s9_7_0032
//! doc: spec
//! section: 9.7
//! title: a member store of a same-block using var into a record local is admitted
//! rule: A record value held at a named location — a local or parameter, a field,
//!   a slot of an inline array held at one — is read with one source set ... a
//!   member store ... adds the sources of the borrows `e` carries to that set,
//!   never removing one, and is checked as an assignment of the updated value to
//!   the location that holds it ... so a source declared in the holder's own block or an enclosing one is
//!   admitted, as at an inline-array slot
//! cites: 9.7, 28.3
//! expect: run
//! stdout: 1
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
    using var it := new Item();
    p.Held := it;
    return p.Held!.N();
  end Run;

  public static method Main()
    Console.WriteLine(Run().ToString());
  end Main;
end Program;
