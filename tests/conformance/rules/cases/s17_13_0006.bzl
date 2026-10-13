//! case: s17_13_0006
//! doc: spec
//! section: 17.13
//! title: a discard of a named owned binding leaves it explicitly-disposed — a later straight-line read warns
//! rule: "`_ := x;` where `x` is a named owned binding is a marker-free
//!   consume ... `x` is thereafter treated exactly as explicitly disposed —
//!   the scope-end pass skips its disposal and frees its block, §28.2's
//!   compiler-inserted free at the binding's release point; a later read
//!   falls under the use-after-dispose regime of §28.2."
//! cites: 17.13, 28.2, 28.3
//! expect: warn
//! diagnostic: disposed
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Widget;

public class Program
  public static method Demo()
    var w := new Widget();
    _ := w;
    var text := w.ToString();
  end Demo;
end Program;
