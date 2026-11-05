//! case: s22_15_2111
//! doc: spec
//! section: 22.15
//! title: a pure constructor chained to a primary over an imported MyError
//!   idiom class is admitted
//! rule: A pure constructor's `base(…)`/`this(…)` chain target must itself
//!   be pure.
//! cites: 22.15, 22.10, 2.8
//! expect: run
//! output: caught d
//! notes: Green on base and on the staging tip: the over-rejection control,
//!   an imported class whose only impure constructor is the unused
//!   convenience one.
module XMc;
import XLC;

class E(m: string): MyError(m)
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
