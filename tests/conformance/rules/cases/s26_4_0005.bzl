//! case: s26_4_0005
//! doc: spec
//! section: 26.4
//! title: the same extension member called at a borrow-free element type is admitted
//! rule: Each member of a generic extension block is read, for §3.6 rule 2,
//!   §14.1 and §14.2, as a **generic method that declares the block's
//!   parameters before its own** — bound by the receiver at a call or a
//!   property read ... a requirement a member body carries on a block
//!   parameter is checked at each such call, read or formation as §3.6 rule 2
//!   checks a method's own parameter
//! cites: 26.4, 3.6, 9.7
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public record Pair(It: Item);

public record Tag(N: int);

public extension Array<T>
  public method Erase(x: T): int
    var o: Object := x;
    return this.Count;
  end Erase;
end;

public class User
  public static method Run(it: Item): int
    var xs := new Array<Tag>();
    var p := new Tag(1);
    return xs.Erase(p);
  end Run;
end User;
