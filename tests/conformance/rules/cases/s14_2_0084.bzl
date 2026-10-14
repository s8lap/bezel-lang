//! case: s14_2_0084
//! doc: spec
//! section: 14.2
//! title: an override omitting the where clause inherits the base's constraint and is admitted
//! rule: An `override` of a generic method ... inherits the overridden
//!   declaration's constraints. Omitting the `where` clauses is the
//!   recommended spelling and means exactly the inherited set (v0.338,
//!   §14.2). `C` overrides `B<int>.Q<U>` under `where U: IComparable<U>`,
//!   and the call `b.Q<int>` satisfies it, since `int` implements
//!   `IComparable<int>`.
//! cites: 14.2, 12.1
//! expect: run
//! stdout: 2
module Conf;

public class B<T>
  public constructor()
  end constructor;

  public virtual method Q<U>(e: U, t: T): int where U: IComparable<U> => 1;
end B;

public class C: B<int>
  public constructor()
  end constructor;

  public override method Q<V>(e: V, t: int): int => 2;
end C;

public class Program
  public static method Main(): int
    var b: B<int> := new C();
    Console.WriteLine(b.Q<int>(4, 1));
    return 0;
  end Main;
end Program;
