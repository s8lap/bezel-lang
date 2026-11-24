//! case: s22_3_8602
//! doc: spec
//! section: 22.3
//! title: two successive lambda writes through a captured generic ref p: T leave the last value in the caller
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are visible through the caller's location immediately
//! cites: 22.3, 27.5, 14.1
//! expect: run
//! stdout: 890
//! notes: Harvested from the #9077 review (probe c1_twice). The lambda writes p := q then p := r; the caller observes the last write, "r{j}", whose length is 1+digits(j). Each iteration adds 1 (lambda result) + 1 + digits(j); the total is 200*2+490 = 890.
module Conf;
import System;
public class Program
  public static method M<T>(ref p: T, q: T, r: T): int
    var w := lambda(): int => do p := q; p := r; return 1; end;
    var a := w();
    return a;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s: string := $"s{j}";
      t := t + Program.M<string>(ref s, $"q{j}", $"r{j}");
      t := t + s.Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
