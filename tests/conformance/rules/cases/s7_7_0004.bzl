//! case: s7_7_0004
//! doc: spec
//! section: 7.7
//! title: owned ?? owned is owned and the chosen value is discharged once
//! rule: When the operands are owned (or owned-nullable), `owned ?? owned` is
//!   owned — the chosen value is discharged once.
//! cites: 28.3
//! expect: run
//! stdout: chosen
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method MaybeMake(flag: bool): owned Widget?
    if flag then
      return new Widget();
    end;
    return null;
  end MaybeMake;

  public static method Main()
    using var w := MaybeMake(true) ?? new Widget();
    Console.WriteLine("chosen");
  end Main;
end Program;
