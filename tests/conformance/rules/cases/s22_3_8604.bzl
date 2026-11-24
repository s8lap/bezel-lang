//! case: s22_3_8604
//! doc: spec
//! section: 22.3
//! title: a reader and a writer lambda over one captured generic ref p: T share its location
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are visible through the caller's location immediately
//! cites: 22.3, 27.5, 14.1
//! expect: run
//! stdout: 1090
//! notes: Harvested from the #9077 review (probe r08_ref_tstr_two_envs; the review's 890 headline does not apply to this probe, its own matrix prints 1090). r reads p through Use<T>, w writes p := q; both capture the same location (§27.5). Use returns 1, so M returns 1+1 = 2 per iteration, and the caller adds s.Length = 1+digits(j): 3+digits(j) per iteration, total 200*3+490 = 1090.
module Conf;
import System;
public class Program
  public static method Use<T>(x: T): int
    var y := x;
    return 1;
  end Use;
  public static method M<T>(ref p: T, q: T): int
    var r := lambda(): int => Program.Use<T>(p);
    var w := lambda(): int => do p := q; return 1; end;
    var a := w();
    return a + r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s := $"s{j}";
      t := t + Program.M<string>(ref s, $"q{j}");
      t := t + s.Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
