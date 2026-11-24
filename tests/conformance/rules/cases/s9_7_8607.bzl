//! case: s9_7_8607
//! doc: spec
//! section: 9.7
//! title: an escaping closure that assigns a string? parameter from another reads the stored value
//! rule: runtime-managed values have unbounded lifetime by construction, so no borrow-lifetime rules apply
//! cites: 9.7, 28.9, 27.5
//! expect: run
//! stdout: 1780
//! notes: Harvested from the #9069 review (probe p08_write_in_lambda_single). The closure runs p := q and returns the length of p; both calls see "q-{j}" (length 2+digits(j)). Per iteration 2*(2+d); sum = 2*(400+490) = 1780.
module Conf;
import System;
public class Program
  public static method M(p: string?, q: string?): () -> int
    return lambda(): int => do p := q; return (p ?? "").Length; end;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.M($"fr-{j}", $"q-{j}"); t := t + f() + f();
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
