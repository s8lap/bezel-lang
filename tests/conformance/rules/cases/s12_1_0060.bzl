//! case: s12_1_0060
//! doc: spec
//! section: 12.1
//! title: an override declaring a type parameter where the base member declares none is rejected
//! rule: An override declares exactly as many type parameters of its own as
//!   the overridden member (none where that member declares none), the two
//!   lists corresponding by position (v0.338, §12.1).
//! cites: 12.1
//! expect: reject
//! diagnostic: type parameter
module Conf;

public class B<T>
  public constructor()
  end constructor;

  public virtual method Q(t: T): int => 1;
end B;

public class C: B<int>
  public constructor()
  end constructor;

  public override method Q<V>(t: int): int => 2;
end C;

public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
