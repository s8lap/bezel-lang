//! case: s22_3_8601
//! doc: spec
//! section: 22.3
//! title: a lambda write through a captured generic ref p: T at string is visible to the caller
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are visible through the caller's location immediately
//! cites: 22.3, 27.5, 14.1
//! expect: run
//! stdout: 890
//! notes: Harvested from the #9077 review (probe a1_string, the issue repro). M<T> captures its ref parameter p in a lambda that writes q into it; the capture names the parameter's own location (§27.5), so the caller's s reads back "q{j}". Per iteration the lambda returns 1 and s.Length = 1+digits(j); digits(j) sums to 10*1+90*2+100*3 = 490 over j in 0..199, so the total is 200*2+490 = 890.
module Conf;
import System;
public class Program
  public static method M<T>(ref p: T, q: T): int
    var w := lambda(): int => do p := q; return 1; end;
    var a := w();
    return a;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s: string := $"s{j}";
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
