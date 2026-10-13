//! case: s3_6_0012
//! doc: spec
//! section: 3.6
//! title: a T? field at a borrow-bearing substitution holds borrows and rejects the store
//! rule: For borrow-bearing, holds-borrows, ... a nullable type `V?` is read as
//!   `V`, nullability stripped ... A rule that reads one of these properties
//!   off a declared, component or substituted type — §9.7's borrow-lifetime
//!   bullets ... — reads `V?` as it reads `V`, whether the nullable form is
//!   written or manufactured by substitution (`T?` at `T = Pair`, or `T` at
//!   `T = Pair?`)
//! cites: 3.6, 9.7
//! expect: reject
//! diagnostic: borrow
//! issue: 5823
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public record Pair(It: Item);

public class Keep<T>
  private _last: T?;

  public constructor()
    _last := null;
  end constructor;

  public method Put(a: T)
    _last := a;
  end Put;
end Keep;

public class User
  public static method Run(p: Pair): int
    using var k := new Keep<Pair>();
    k.Put(p);
    return 1;
  end Run;
end User;
