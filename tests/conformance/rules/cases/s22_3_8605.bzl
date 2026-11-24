//! case: s22_3_8605
//! doc: spec
//! section: 22.3
//! title: a reader lambda sees the value a writer lambda stored through a captured concrete ref p: string
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are visible through the caller's location immediately
//! cites: 22.3, 27.5, 14.1
//! expect: run
//! stdout: 1580
//! notes: Harvested from the #9077 review (probe r08n_ref_str_twin, the concrete-string twin of s22_3_8604). r returns p.Length after w has written p := q, so it reads the alias: len(q)=1+digits(j). M returns 1+(1+digits(j)), the caller adds s.Length = 1+digits(j): 3+2*digits(j) per iteration, total 200*3+2*490 = 1580.
module Conf;
import System;
public class Program
  public static method M(ref p: string, q: string): int
    var r := lambda(): int => p.Length;
    var w := lambda(): int => do p := q; return 1; end;
    var a := w();
    return a + r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s := $"s{j}";
      t := t + Program.M(ref s, $"q{j}");
      t := t + s.Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
