//! case: s28_7_9902
//! doc: spec
//! section: 28.7
//! title: an owned Set<string?> field assigned a returned owned set in a 200x loop keeps its count
//! rule: Assignment to an owned field or owned local disposes the displaced value
//! cites: 28.7, 28.11, 3.6
//! expect: run
//! stdout: 400
//! issue: 9263
//! notes: Harvested from the #9263 review (p4_returned). Each Take displaces the previous set; every set
//!   holds a string and null, so each pass adds 2: 400.
module Conf;

public class Holder
  public owned _s: Set<string?> := new Set<string?>();

  public static method Make(i: int): owned Set<string?>
    var n := new Set<string?>();
    n.Add("m" + i.ToString());
    n.Add(null);
    return n;
  end Make;

  public method Take(i: int)
    _s := Make(i);
  end Take;

  public method Cnt(): int
    return _s.Count;
  end Cnt;
end Holder;

public class Program
  public static method Main()
    var tot := 0;
    var h := new Holder();
    for var i in 0 ..< 200 do
      h.Take(i);
      tot := tot + h.Cnt();
    end for;
    Console.WriteLine(tot);
  end Main;
end Program;
