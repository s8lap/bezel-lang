//! case: s27_5_8014
//! doc: spec
//! section: 27.5
//! title: an instance method's returned closure growing a captured string parameter yields
//!   growing lengths
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!   vice versa.
//! cites: 27.5, 28.9, 22.3
//! expect: run
//! stdout: 3000
//! notes: Pins #9005 (a second closure writing a captured string parameter freed a string
//!   the caller still holds); output only, the corpus runner has no sanitizer leg.
module Conf;
public class Box
  public method M(p: string): () -> () -> string
    return lambda() => lambda(): string => do p := p + "z"; return p; end;
  end M;
end Box;
public class Program
  public static method Run(): int
    var s := $"hi-{1}";
    var b := new Box();
    var t := 0;
    for var j in 0..<200 do
      var f := b.M(s);
      t := t + f()().Length + f()().Length + s.Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
