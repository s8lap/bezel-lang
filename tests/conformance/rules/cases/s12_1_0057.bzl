//! case: s12_1_0057
//! doc: spec
//! section: 12.1
//! title: an indexer override with a concrete element type dispatches through a generic base
//! rule: A method, property, or indexer is non-virtual unless marked
//!   `virtual`. To override a virtual member in a derived class, mark the
//!   matching member `override`. The indexer `[i: int]: T` of `B<string>`
//!   has element type `string`, so `override property [i: int]: string`
//!   matches it (v0.338, §12.1), and a call through `B<string>` reaches the
//!   override.
//! cites: 12.1, 14.1
//! expect: run
//! issue: 6304
//! stdout: override
module Conf;

public class B<T>
  public constructor()
  end constructor;

  public virtual property [i: int]: T => default(T);
end B;

public class E: B<string>
  public constructor()
  end constructor;

  public override property [i: int]: string => "override";
end E;

public class Program
  public static method Main(): int
    var b: B<string> := new E();
    Console.WriteLine(b[1]);
    return 0;
  end Main;
end Program;
