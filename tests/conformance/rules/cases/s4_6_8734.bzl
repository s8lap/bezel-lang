//! case: s4_6_8734
//! doc: spec
//! section: 4.6
//! title: a generic identity lambda at a nullable string substitution passes string and null
//!   through
//! rule: `string` is **runtime-managed**: its memory is reclaimed automatically.
//! cites: 4.6, 28.9, 27.3
//! expect: run
//! stdout: 1290
//! notes: Pins #9118 (lambda parameter). `Id<string?>` returns `(x: T) => x`. Per iteration the
//!   string "fr-j" comes back (lengths as in s4_6_8728: 1090 in total) and `f(null)` comes
//!   back null, whose `?? "x"` length is 1: +200. Total 1290.
module Conf;
public class Program
  public static method Id<T>(): (T) -> T
    return lambda(x: T): T => x;
  end Id;
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 200 do
      var f := Program.Id<string?>();
      var r := f($"fr-{j}");
      t := t + (r ?? "").Length;
      var z := f(null);
      t := t + (z ?? "x").Length;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine($"{Program.Run()}");
    return 0;
  end Main;
end Program;
