//! case: s9_7_8601
//! doc: spec
//! section: 9.7
//! title: an escaping closure that reads a captured string? parameter twice keeps the value alive
//! rule: runtime-managed values have unbounded lifetime by construction, so no borrow-lifetime rules apply
//! cites: 9.7, 28.9, 27.5
//! expect: run
//! output: 2180
//! notes: Harvested from the #9069 review (probe p10b_two_closures_ro, the genuine two-capturer shape). M returns r, which captures the by-value string? parameter p and also calls a, a second closure over p. The string value has unbounded lifetime, so both reads see "fr-{j}" (length 3+digits(j)) after M has returned. r() = 2*(3+d); sum over j in 0..199 = 2*(600+490) = 2180.
module Conf;
import System;
public class Program
  public static method M(p: string?): () -> int
    var a := lambda(): int => (p ?? "").Length;
    var r := lambda(): int => (p ?? "").Length + a();
    return r;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.M($"fr-{j}"); t := t + f();
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
