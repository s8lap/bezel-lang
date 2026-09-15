//! case: s20_0008
//! doc: spec
//! section: 20
//! title: a bare null branch target-types to the nullable form of the other branch's type
//! rule: "**Bare `null`.** A bare `null` branch likewise imposes no constraint of
//!   its own: it target-types to the nullable form of the other branches' common
//!   type."
//! cites: 20, 28.3
//! expect: run
//! stdout: null
module Conf;

public class Widget
  public constructor()
  end constructor;

  public override method ToString(): string => "widget";
end Widget;

public class Program
  public static method Main()
    var present := false;
    using var w := (if present then new Widget() else null);
    Console.WriteLine($"{w}");
  end Main;
end Program;
