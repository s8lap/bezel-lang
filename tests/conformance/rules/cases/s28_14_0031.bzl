//! case: s28_14_0031
//! doc: spec
//! section: 28.14
//! title: overwriting the field under a scoped construction faults the stored borrow in checked mode
//! rule: The bound is on the source, not on what a slot read through it currently
//!   holds: an argument whose borrow is of a field's, property's or slot's
//!   current value — `new Reader(scoped _s)` under the `this` arm, `scoped owner.Child`
//!   under a parameter or local `owner` — is admitted on its source's standing, and a later overwrite, `move` or disposal of that slot
//!   (`_s := new Stream();`, `owner.Reset()`) is K5.6's posture ... a member access
//!   through the stored borrow afterwards is K4.1's deterministic
//!   `DisposedAccessException` in checked disposal mode
//! cites: 28.14, 28.2, 28.7
//! expect: run
//! stdout: faulted
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

  public method Pump()
    using var r := new Reader(scoped _s);
    _s := new Source();
    try
      Console.WriteLine(r.Next().ToString());
    catch
      DisposedAccessException => Console.WriteLine("faulted"),
    end try;
  end Pump;
end Host;

public class Program
  public static method Main()
    using var h := new Host();
    h.Pump();
  end Main;
end Program;
