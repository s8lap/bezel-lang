//! case: s22_15_2107
//! doc: spec
//! section: 22.15
//! title: a pure constructor chained to a primary whose base call selects
//!   the pure int constructor is admitted
//! rule: A pure constructor's `base(…)`/`this(…)` chain target must itself
//!   be pure.
//! cites: 22.15, 22.10
//! expect: run
//! output: caught i
//! notes: Green on base and on the staging tip: the over-rejection control
//!   for the #8163 primary-chain purity check.
module M;

class B : Exception
  public pure constructor(x: int) base("i");
  end constructor;

  public constructor(x: long) base("l");
    Console.WriteLine("IMPURE long");
  end constructor;
end B;

class E(m: string): B(5)
  public pure constructor() this("d");
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
