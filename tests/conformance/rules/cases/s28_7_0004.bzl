//! case: s28_7_0004
//! doc: spec
//! section: 28.7
//! title: an explicit Dispose before reassignment composes with drop-on-overwrite as a no-op
//! rule: An explicit `field.Dispose();` before the assignment remains legal
//!   and behavior-preserving — Dispose is idempotent, so the implicit
//!   disposal of an already-disposed value is a no-op.
//! cites: 28.7, 28.2
//! expect: run
//! output: dispose first
//!   done
//!   dispose second
module Conf;

public class Widget
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    Console.WriteLine($"dispose {_name}");
  end destructor;
end Widget;

public class Holder
  private owned _w: Widget;

  public constructor()
    _w := new Widget("first");
  end constructor;

  public method ReplaceExplicit()
    _w.Dispose();
    _w := new Widget("second");
  end ReplaceExplicit;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    h.ReplaceExplicit();
    Console.WriteLine("done");
  end Main;
end Program;
