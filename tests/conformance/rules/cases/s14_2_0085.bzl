//! case: s14_2_0085
//! doc: spec
//! section: 14.2
//! title: an override of a generic method that adds a where clause the inherited set lacks is rejected
//! rule: A differing clause — added, removed, narrowed, or widened relative to
//!   the inherited set — is a compile error naming the inherited constraints
//!   (v0.338, §14.2): dispatch flows through the base signature, so an
//!   added constraint on the override would go unenforced at every virtual
//!   call. `C`'s override adds `where V: IComparable<V>` to a base method
//!   that declares no constraint.
//! cites: 14.2, 12.1
//! expect: reject
//! diagnostic: constraint
module Conf;

public class B<T>
  public constructor()
  end constructor;

  public virtual method Q<U>(e: U, t: T): int => 1;
end B;

public class C: B<int>
  public constructor()
  end constructor;

  public override method Q<V>(e: V, t: int): int where V: IComparable<V> => 2;
end C;

public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
