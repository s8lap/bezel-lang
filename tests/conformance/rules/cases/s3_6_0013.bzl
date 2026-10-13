//! case: s3_6_0013
//! doc: spec
//! section: 3.6
//! title: the same T? field store is admitted at a substitution that holds no borrows
//! rule: For borrow-bearing, holds-borrows, ... a nullable type `V?` is read as
//!   `V`, nullability stripped ... A rule that reads one of these properties
//!   off a declared, component or substituted type — §9.7's borrow-lifetime
//!   bullets ... — reads `V?` as it reads `V`
//! cites: 3.6, 9.7
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public record Pair(It: int);

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
