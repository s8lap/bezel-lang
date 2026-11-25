//! case: s28_11_9902
//! doc: spec
//! section: 28.11
//! title: control: an owned Set<string> field cleared, refilled and overwritten keeps its counts
//! rule: Every standard collection's element semantics follow from one classification
//! cites: 28.11, 28.7
//! expect: run
//! stdout: 800
//! issue: 9263
//! notes: Harvested from the #9263 review (p2_nonnull): the non-nullable twin of s28_7_9901, green before
//!   and after the fix. Per pass Fill leaves 2, Replace 1, the second Replace 1: 4 per pass, 800.
module Conf;

public class Holder
  public owned _s: Set<string> := new Set<string>();

  public method Fill(i: int)
    _s.Add("a" + i.ToString());
    _s.Add("b" + i.ToString());
  end Fill;

  public method Clr()
    _s.Clear();
  end Clr;

  public method Replace(i: int)
    var n := new Set<string>();
    n.Add("r" + i.ToString());
    _s := n;
  end Replace;

  public method Cnt(): int
    return _s.Count;
  end Cnt;
end Holder;

public class Program
  public static method Main()
    var tot := 0;
    for var i in 0 ..< 200 do
      var h := new Holder();
      h.Fill(i);
      tot := tot + h.Cnt();
      h.Clr();
      h.Fill(i);
      h.Replace(i);
      tot := tot + h.Cnt();
      h.Clr();
      h.Replace(i);
      h.Replace(i + 1);
      tot := tot + h.Cnt();
    end for;
    Console.WriteLine(tot);
  end Main;
end Program;
