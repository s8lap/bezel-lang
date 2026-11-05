//! case: s22_15_2103
//! doc: spec
//! section: 22.15
//! title: pure ctor chained to a primary whose generic base call picks the
//!   impure constructor is rejected
//! rule: A pure constructor's `base(…)`/`this(…)` chain target must itself
//!   be pure.
//! cites: 22.15, 22.10
//! expect: reject
//! diagnostic: pure
//! notes: The pre-fix compiler fails this program with an internal IR
//!   verification error instead of the purity diagnostic, so the runner's
//!   reject check cannot tell it from a correct rejection there. Only the
//!   staging tip gives the 22.15 diagnostic.
module M;

class GB<T> : Exception
  public constructor(x: T) base("t");
    Console.WriteLine("IMPURE T");
  end constructor;

  public pure constructor(x: int, y: int) base("i");
  end constructor;
end GB;

class E(m: string): GB<string>(m)
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
