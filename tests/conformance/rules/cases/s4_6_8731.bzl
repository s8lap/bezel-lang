//! case: s4_6_8731
//! doc: spec
//! section: 4.6
//! title: a generic class method lambda result at a nullable string substitution is read like any
//!   string
//! rule: `string` is **runtime-managed**: its memory is reclaimed automatically.
//! cites: 4.6, 28.9, 14.1
//! expect: run
//! stdout: 1090
//! notes: Pins #9118 (generic class). `G<string?>.Mk` returns a closure over its parameter. The
//!   lengths of "fr-0" .. "fr-199" are 4 for j < 10, 5 for 10 <= j < 100 and 6 for 100 <= j <
//!   200: 40 + 450 + 600 = 1090.
module Conf;
public class G<T>
  public method Mk(p: T): () -> T
    return lambda(): T => p;
  end Mk;
end G;
public class Program
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 200 do
      var g := new G<string?>();
      var f := g.Mk($"fr-{j}");
      var r := f();
      t := t + (r ?? "").Length;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine($"{Program.Run()}");
    return 0;
  end Main;
end Program;
