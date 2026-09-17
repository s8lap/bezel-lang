//! case: s22_6_0014
//! doc: spec
//! section: 22.6
//! title: a move-marked owned production selects the owned member of an owned-ordinary pair
//! rule: "An owned production is applicable only to the `owned` candidate:
//!   a move-rooted argument (`move x` ...) ... `r.Append(move w);` //
//!   transfer — the owned overload."
//! cites: 22.6
//! expect: run
//! stdout: take
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
