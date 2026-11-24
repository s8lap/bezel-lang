//! case: s9_7_8604
//! doc: spec
//! section: 9.7
//! title: an escaping closure that returns its captured string? parameter by name returns the original value
//! rule: runtime-managed values have unbounded lifetime by construction, so no borrow-lifetime rules apply
//! cites: 9.7, 28.9, 27.5
//! expect: run
//! stdout: 1090
//! notes: Harvested from the #9069 review (probe p18_nstr_returned_by_name). The closure's body is the bare name p; after M has returned, f() yields the argument "fr-{j}", whose length 3+digits(j) is added. Sum = 1090.
module Conf;
import System;
public class Program
  public static method M(p: string?): () -> string?
    return lambda(): string? => p;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.M($"fr-{j}"); t := t + (f() ?? "").Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
