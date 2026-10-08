//! case: s9_7_0020
//! doc: spec
//! section: 9.7
//! title: a where T: value identity is admitted at int, the bullet being read per instantiation
//! rule: Over `record Pair(Item: Item)`, `static method Pass<T>(owned a: T):
//!   owned T => a;` is therefore rejected at `Pass<Pair>`, its return
//!   carrying a borrow of an `owned` parameter, and so are ... and `static
//!   method Id<T>(a: T): T where T: value => a;` at `Id<Pair>`; each stays
//!   admitted at the declaration and at every substitution that carries no
//!   borrow.
//! cites: 9.7, 3.6
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public record Pair(Item: Item);

public class Fns
  public static method Id<T>(a: T): T where T: value => a;
end Fns;

public class Client
  public static method Run(n: int): int
    return Fns.Id<int>(n);
  end Run;
end Client;
