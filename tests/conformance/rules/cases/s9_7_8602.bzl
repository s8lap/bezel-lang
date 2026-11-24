//! case: s9_7_8602
//! doc: spec
//! section: 9.7
//! title: a nested escaping closure over a string? parameter reads the value after the method returned
//! rule: runtime-managed values have unbounded lifetime by construction, so no borrow-lifetime rules apply
//! cites: 9.7, 28.9, 27.5
//! expect: run
//! stdout: 1090
//! notes: Harvested from the #9069 review (probe p11_nested). M returns a closure that returns a closure over p; the inner closure reads the length of "fr-{j}" = 3+digits(j). Sum over j = 600+490 = 1090.
module Conf;
import System;
public class Program
  public static method M(p: string?): () -> () -> int
    return lambda(): () -> int => lambda(): int => (p ?? "").Length;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var g := Program.M($"fr-{j}"); var f := g(); t := t + f();
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
