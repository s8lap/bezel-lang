//! case: s22_6_0014
//! doc: spec
//! section: 22.6
//! title: a move-marked production of a same-type owned/ordinary pair is rejected at the declaration (v0.338)
//! rule: **`owned` never distinguishes overloads (v0.338, proposal 0185).**
//!   `Append(item: Widget)` beside `Append(owned item: Widget)` clash at the
//!   second declaration (§22.6, §11.3); the owned production is not selected.
//! cites: 22.6
//! expect: reject
//! diagnostic: `owned` and `scoped` never distinguish overloads
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Registry
  public constructor()
  end constructor;

  public method Append(item: Widget): string => "borrow";
  public method Append(owned item: Widget): string => "take";
end Registry;

public class Program
  public static method Main()
    using var r := new Registry();
    var w := new Widget();
    Console.WriteLine(r.Append(move w));
  end Main;
end Program;
