//! case: s9_7_8605
//! doc: spec
//! section: 9.7
//! title: an async method returns a closure over its string? parameter that is read after the await
//! rule: runtime-managed values have unbounded lifetime by construction, so no borrow-lifetime rules apply
//! cites: 9.7, 28.9, 27.5
//! expect: run
//! stdout: 1090
//! notes: Harvested from the #9069 review (probe p21b_async_nstr_esc). M awaits, then returns a closure over the string? parameter p; the caller reads p through f() after M's frame is gone: length 3+digits(j) per iteration, sum 1090.
module Conf;
import System;
import System.Threading;
public class Program
  public static async method M(p: string?): Task<() -> int>
    await Task.Delay(1);
    return lambda(): int => (p ?? "").Length;
  end M;
  public static async method Main(): Task<int>
    var t := 0;
    for var j in 0..<200 do
      var f := await Program.M($"fr-{j}");
      t := t + f();
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
