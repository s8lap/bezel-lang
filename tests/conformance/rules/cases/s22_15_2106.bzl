//! case: s22_15_2106
//! doc: spec
//! section: 22.15
//! title: a pure constructor at the top of a five-level chain ending in an
//!   impure constructor is rejected
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

class IB : Exception
  public constructor(m: string) base(m);
    Console.WriteLine("IMPURE");
  end constructor;
end IB;

class C1(m: string): IB(m)
end C1;

class C2(m: string): C1(m)
end C2;

class C3(m: string): C2(m)
end C3;

class E(m: string): C3(m)
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
