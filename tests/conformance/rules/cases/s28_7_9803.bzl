//! case: s28_7_9803
//! doc: spec
//! section: 28.7
//! title: Overwriting an owned LinkedList<string?> field after elements were removed from both lists, 200x
//! rule: Assignment to an owned field or owned local disposes the displaced value
//! cites: 28.7, 28.11
//! expect: run
//! output: 200
//! notes: Pins #9149 (test pick 12194bf70, fix 7c5182e2e): overwriting an owned LinkedList<string?> field
//!   disposes the displaced list and releases its string elements exactly once. Each replacement list
//!   has 3 elements, RemoveFirst leaves 2, the owned-field store disposes the displaced list, Pop
//!   leaves 1: Count is 1 per iteration, 200 over the loop.
module Conf;

public class Holder
  private owned _r: LinkedList<string?> := new LinkedList<string?>();
  public method Replace(i: int)
    var n := new LinkedList<string?>();
    n.Add("w" + i.ToString());
    n.Add(null);
    n.Add("x" + i.ToString());
    n.RemoveFirst();
    _r := n;
  end Replace;
  public method Pop()
    _r.RemoveFirst();
  end Pop;
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
      h.Pop();
      tot := tot + h.Cnt();
      i := i + 1;
    end while;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
