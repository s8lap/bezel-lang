//! case: s7_5_0002
//! doc: spec
//! section: 7.5
//! title: a null receiver skips the whole chain, including argument expressions
//! rule: If a null receiver is found, nothing to the right of it in the
//!   chain is evaluated — no member is read, no call is performed, and the
//!   argument expressions of the skipped suffixes are not evaluated:
//!   `user?.Log(Render(x))` calls neither `Log` nor `Render` when `user` is
//!   null.
//! expect: run
//! stdout: done
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Announce(text: string) => Console.WriteLine($"widget: {text}");
end Widget;

public class Program
  public static method Render(): string
    Console.WriteLine("rendered");
    return "R";
  end Render;

  public static method Main()
    var w: Widget? := null;
    w?.Announce(Render());
    Console.WriteLine("done");
  end Main;
end Program;
