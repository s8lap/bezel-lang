//! case: s22_3_8603
//! doc: spec
//! section: 22.3
//! title: a lambda that writes a captured generic ref p: T inside a loop leaves the last value in the caller
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are visible through the caller's location immediately
//! cites: 22.3, 27.5, 14.1
//! expect: run
//! stdout: 890
//! notes: Harvested from the #9077 review (probe c3_loop). The lambda assigns p := r three times in a for loop; the caller observes "r{j}". Per iteration 1 + (1+digits(j)); total 200*2+490 = 890.
module Conf;
import System;
public class Program
  public static method M<T>(ref p: T, q: T, r: T): int
    var w := lambda(): int => do for var k in 0..<3 do p := r; end; return 1; end;
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
