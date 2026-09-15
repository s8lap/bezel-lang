//! case: s28_5_0018
//! doc: spec
//! section: 28.5
//! title: a null owned field disposes nothing
//! rule: A nullable owned field is null-checked before disposal, and a null
//!   field disposes nothing.
//! cites: 28.5
//! expect: run
//! stdout: built
module Conf;

public class Widget
  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public class Holder
  private owned _a: Widget;
  private owned _b: Widget?;

  public constructor()
    _a := new Widget();
    _b := null;
  end constructor;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    Console.WriteLine("built");
  end Main;
end Program;
