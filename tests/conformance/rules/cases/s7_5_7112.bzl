//! case: s7_5_7112
//! doc: spec
//! section: 7.5
//! title: `?.FirstKey?.Length`, a discard, a nullable return and assignment over a SortedDictionary
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 7.7
//! expect: run
//! stdout: 5400
module Conf;
public class Program
  public static method Ret(d: SortedDictionary<string, int>?): string?
    return d?.FirstKey;
  end Ret;
  public static method Chain(d: SortedDictionary<string, int>?): int
    var total := 0;
    total := total + (d?.FirstKey?.Length ?? 0);
    _ := d?.LastKey;
    var r := Ret(d);
    if r <> null then total := total + r.Length; end;
    var s: string? := null;
    s := d?.LastKey;
    if s <> null then total := total + s.Length; end;
    var u := (d?.FirstKey ?? "none").ToUpper();
    total := total + u.Length;
    return total;
  end Chain;
  public static method Main()
    using var d := new SortedDictionary<string, int>();
    d.Add("pear" + "x", 1);
    d.Add("apple" + "y", 2);
    var sum := 0;
    for var i in 0..<200 do
      sum := sum + Chain(d);
      sum := sum + Chain(null);
    end;
    Console.WriteLine(sum);
  end Main;
end Program;
