//! case: s22_15_2108
//! doc: spec
//! section: 22.15
//! title: pure ctor over a three-level primary chain with impure
//!   convenience constructors is admitted
//! rule: A pure constructor's `base(…)`/`this(…)` chain target must itself
//!   be pure.
//! cites: 22.15, 22.10
//! expect: run
//! output: caught e
//! notes: Green on base and on the staging tip: the over-rejection control
//!   for the #8163 primary-chain purity check.
module M;

class A(m: string): Exception(m)
  public constructor() this("a");
    Console.WriteLine("IMPURE A");
  end constructor;
end A;

class B(m: string): A(m)
  public constructor() this("b");
    Console.WriteLine("IMPURE B");
  end constructor;
end B;

class E(m: string): B(m)
  public pure constructor() this("e");
  end constructor;
end E;

class Program
  public static pure method Run(x: int): int
    if x < 0 then throw new E(); end;
    return x;
  end Run;

  public static method Main(): int
    try
      Console.WriteLine(Run(-1));
    catch var e: Exception =>
      Console.WriteLine("caught " + e.Message);
    end;
    return 0;
  end Main;
end Program;
