//! case: s28_14_0030
//! doc: spec
//! section: 28.14
//! title: a scoped construction over a this-rooted field's current value is admitted
//! rule: The bound is on the source, not on what a slot read through it currently
//!   holds: an argument whose borrow is of a field's, property's or slot's
//!   current value — `new Reader(scoped _s)` under the `this` arm, `scoped owner.Child`
//!   under a parameter or local `owner` — is admitted on its source's standing
//! cites: 28.14, 28.3
//! expect: run
//! stdout: 7
module Conf;

public class Source
  public constructor()
  end constructor;

  public method Read(): int => 7;
end Source;

public class Reader
  private _src: Source;

  public constructor(scoped src: Source)
    _src := src;
  end constructor;

  public method Next(): int => _src.Read();
end Reader;

public class Host
  private owned _s: Source;

  public constructor()
    _s := new Source();
  end constructor;

  public method Pump(): int
    using var r := new Reader(scoped _s);
    return r.Next();
  end Pump;
end Host;

public class Program
  public static method Main()
    using var h := new Host();
    Console.WriteLine(h.Pump().ToString());
  end Main;
end Program;
