//! case: s14_4_0007
//! doc: spec
//! section: 14.4
//! title: an owning array of T? is admitted in a generic type whose constraints guarantee class
//! rule: So `OwningTask<T?>` or `OwningArray<T?>` formed in a generic body is
//!   admitted at the declaration exactly where `OwningArray<T>` is — under a
//!   constraint set that guarantees `class` (§14.2: `where T: class`, a class
//!   bound, a bound naming `IDisposable` or an interface extending it) — and
//!   decided at `T`'s substitutions as every instantiation-site rule is (§3.6
//!   rule 2); under any other constraint set, an unconstrained `T` included,
//!   both are rejected at the declaration.
//! cites: 14.4, 14.2, 3.6
//! expect: accept
module Conf;

public interface IShape
  method Area(): int;
end IShape;

public class Conn: IShape
  public constructor()
  end constructor;

  public method Area(): int => 1;
end Conn;

public class Pool<T> where T: class
  private owned _items: OwningArray<T?> := new();

  public constructor()
  end constructor;
end Pool;

public class User
  public static method Run(p: Pool<Conn>): int => 1;
end User;
