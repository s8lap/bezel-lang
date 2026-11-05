//! case: s22_15_2105
//! doc: spec
//! section: 22.15
//! title: a pure constructor chained to a primary whose base primary
//!   invokes an impure constructor is rejected
//! rule: A pure constructor's `base(…)`/`this(…)` chain target must itself
//!   be pure.
//! cites: 22.15, 22.10
//! expect: reject
//! diagnostic: pure
//! issue: 8163
//! notes: Red on base: the pre-fix compiler admitted the pure constructor
//!   although the primary constructor it chains to invokes an impure base
//!   constructor, so the program compiled and ran the impure body from a
//!   pure method. It is rejected on the staging tip.
module M;

class Base : Exception
  public constructor(m: string) base(m);
    Console.WriteLine("IMPURE base");
  end constructor;
end Base;

class Mid(m: string): Base(m)
  public pure constructor(n: int) this("x");
  end constructor;
end Mid;

class E(m: string): Mid(m)
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
