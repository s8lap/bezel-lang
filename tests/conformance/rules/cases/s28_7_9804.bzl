//! case: s28_7_9804
//! doc: spec
//! section: 28.7
//! title: Overwriting an owned nullable LinkedList<string?> field and clearing it to null, 200x
//! rule: Assignment to an owned field or owned local disposes the displaced value
//! cites: 28.7, 28.11
//! expect: run
//! output: ok
//! notes: Pins #9149 (test pick 12194bf70, fix 7c5182e2e): overwriting an owned LinkedList<string?> field
//!   disposes the displaced list and releases its string elements exactly once. Replace stores into a
//!   null location (nothing to dispose); Clear stores null over a live list (disposes it). 200 cycles
//!   then a final Replace; the program completes and prints ok.
module Conf;

public class Holder
  private owned _r: LinkedList<string?>? := null;
  public method Replace(i: int)
    var n := new LinkedList<string?>();
    n.Add("w" + i.ToString());
    n.Add(null);
    _r := n;
  end Replace;
  public method Clear()
    _r := null;
  end Clear;
end Holder;

public class Program
  public static method Main(): int
    using var h := new Holder();
    var i := 0;
    while i < 200 do
      h.Replace(i);
      h.Clear();
      i := i + 1;
    end while;
    h.Replace(1);
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
