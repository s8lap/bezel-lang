//! case: s28_7_9802
//! doc: spec
//! section: 28.7
//! title: Overwriting an owned LinkedList<string?> field keeps a string held elsewhere
//! rule: Assignment to an owned field or owned local disposes the displaced value
//! cites: 28.7, 28.9
//! expect: run
//! output: sh7
//!   600
//! notes: Pins #9149 (test pick 12194bf70, fix 7c5182e2e): overwriting an owned LinkedList<string?> field
//!   disposes the displaced list and releases its string elements exactly once. A string held by the
//!   caller is stored twice into each replacement list; the displaced lists must release only the
//!   references they took, so shared still reads sh7 after 200 overwrites and a further one.
module Conf;

public class Holder
  private owned _r: LinkedList<string?> := new LinkedList<string?>();
  public method Replace(s: string)
    var n := new LinkedList<string?>();
    n.Add(s);
    n.Add(null);
    n.Add(s);
    _r := n;
  end Replace;
  public method Cnt(): int
    return _r.Count;
  end Cnt;
end Holder;

public class Program
  public static method Main(): int
    var tot := 0;
    using var h := new Holder();
    var shared := "sh" + (3 + 4).ToString();
    var i := 0;
    while i < 200 do
      h.Replace(shared);
      tot := tot + h.Cnt();
      i := i + 1;
    end while;
    h.Replace("z" + 1.ToString());
    Console.WriteLine(shared);
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
