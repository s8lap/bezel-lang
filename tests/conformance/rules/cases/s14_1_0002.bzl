//! case: s14_1_0002
//! doc: spec
//! section: 14.1
//! title: a generic type named in receiver position is checked at the substitution there
//! rule: and a generic type named in **receiver position** of a static member
//!   access or at the head of a member chain (§22.12: `Reg<Pair>.Put(x)`,
//!   `Cache<string>.Default.Count`). ... Wherever a rule of this specification
//!   fires *at the instantiation site* — §3.6 rule 2's per-instantiation
//!   property evaluation, ... — it fires at every such site.
//! cites: 14.1, 3.6, 9.7, 22.12
//! expect: reject
//! diagnostic: borrow
//! issue: 5814
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public record Pair(It: Item);

public record Tag(N: int);

public class Reg<T>
  public static method Put(x: T): int
    var o: Object := x;
    return 1;
  end Put;
end Reg;

public class User
  public static method Run(it: Item): int
    var p := new Pair(it);
    return Reg<Pair>.Put(p);
  end Run;
end User;
