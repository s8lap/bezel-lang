//! case: s28_7_0003
//! doc: spec
//! section: 28.7
//! title: reassigning a moved-from field stores without disposal
//! rule: The disposal is null-guarded and dynamically dispatched; a
//!   moved-from or null location therefore stores without disposal, which is
//!   how the move-then-reassign idiom stays single-disposal.
//! cites: 28.7, 28.8.1
//! expect: run
//! stdout: swapped
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

  public method Swap(): owned Widget
    var taken := move _w;
    _w := new Widget("second");
    return taken;
  end Swap;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    using var taken := h.Swap();
    Console.WriteLine("swapped");
  end Main;
end Program;
