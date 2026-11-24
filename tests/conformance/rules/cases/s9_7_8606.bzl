//! case: s9_7_8606
//! doc: spec
//! section: 9.7
//! title: an escaping closure over a string? parameter and a string parameter reads both on repeated calls
//! rule: runtime-managed values have unbounded lifetime by construction, so no borrow-lifetime rules apply
//! cites: 9.7, 28.9, 27.5
//! expect: run
//! stdout: 4360
//! notes: Harvested from the #9069 review (probe p25_nstr_param_with_owned_other). The closure reads the length of p ("fr-{j}", 3+digits(j)) and q ("qq-{j}", 3+digits(j)) and is called twice: 2*(6+2d) = 12+4d per iteration; sum = 200*12+4*490 = 4360.
module Conf;
import System;
public class Program
  public static method M(p: string?, q: string): () -> int
    var a := lambda(): int => (p ?? "").Length + q.Length;
    return a;
  end M;
  public static method Main(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.M($"fr-{j}", $"qq-{j}");
      t := t + f() + f();
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
