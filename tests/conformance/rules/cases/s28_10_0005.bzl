//! case: s28_10_0005
//! doc: spec
//! section: 28.10
//! title: a using var initialized from a borrow does not assume a disposal obligation
//! rule: When the right-hand side of a using var declaration is a borrow
//!   rather than an owned value — a method call without an owned return, an
//!   indexer access, a field read, or an unmarked read of a class- or
//!   interface-typed named owned binding — the using var does not assume a
//!   disposal obligation: at scope end no Dispose() is called, because the
//!   value is not owned by this scope. An unmarked read of a move-only value
//!   binding moves instead and is an owned right-hand side.
//! cites: 28.10, 15.3
//! expect: run
//! output: viewed
//!   after view
//!   dispose held
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
    _w := new Widget("held");
  end constructor;

  public method Borrowed(): Widget => _w;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    do
      using var view := h.Borrowed();
      Console.WriteLine("viewed");
    end;
    Console.WriteLine("after view");
  end Main;
end Program;
