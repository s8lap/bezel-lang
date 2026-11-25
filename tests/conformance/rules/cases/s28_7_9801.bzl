//! case: s28_7_9801
//! doc: spec
//! section: 28.7
//! title: Overwriting an owned LinkedList<string?> field holding strings and a null releases each once, 200x
//! rule: Assignment to an owned field or owned local disposes the displaced value
//! cites: 28.7, 28.11, 3.6
//! expect: run
//! output: 600
//! notes: Pins #9149 (test pick 12194bf70, fix 7c5182e2e): overwriting an owned LinkedList<string?> field
//!   disposes the displaced list and releases its string elements exactly once. Each Replace stores a
//!   three-element list (string, null, string); Count is 3 after every overwrite: 600.
module Conf;

public class Holder
  private owned _r: LinkedList<string?> := new LinkedList<string?>();
  public method Replace(i: int)
    var n := new LinkedList<string?>();
    n.Add("w" + i.ToString());
    n.Add(null);
    n.Add("x" + i.ToString());
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
    var i := 0;
    while i < 200 do
      h.Replace(i);
      tot := tot + h.Cnt();
      i := i + 1;
    end while;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
