//! case: s28_7_9901
//! doc: spec
//! section: 28.7
//! title: an owned Set<string?> field overwritten, cleared and refilled in a 200x loop keeps its counts
//! rule: Assignment to an owned field or owned local disposes the displaced value
//! cites: 28.7, 28.11, 3.6
//! expect: run
//! stdout: 1000
//! issue: 9263
//! notes: Harvested from the #9263 review (p1_field_clear_reassign). Per pass: Fill leaves 3 elements (two
//!   strings and null), Replace leaves 1, the second Replace leaves 1, so 5 per pass and 1000 over
//!   200 passes. The leak half of #9263 needs a sanitizer, which the corpus runner lacks; the pin
//!   here is that the displaced sets are released without disturbing the live one.
module Conf;

public class Holder
  public owned _s: Set<string?> := new Set<string?>();

  public method Fill(i: int)
    _s.Add("a" + i.ToString());
    _s.Add(null);
    _s.Add("b" + i.ToString());
  end Fill;

  public method Clr()
    _s.Clear();
  end Clr;

  public method Replace(i: int)
    var n := new Set<string?>();
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
