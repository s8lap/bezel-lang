//! case: s28_11_9901
//! doc: spec
//! section: 28.11
//! title: a string element shared by an Array<string?> and a Set<string?> field stays readable
//! rule: Every standard collection's element semantics follow from one classification
//! cites: 28.11, 28.7, 3.6
//! expect: run
//! stdout: 1690
//! issue: 9263
//! notes: Harvested from the #9263 review (p3_shared). Both slots are shared-reference slots (the slot
//!   policy: store takes a reference), so the displaced collections giving their references back must
//!   not free the string the live ones still hold. Per pass: the set holds 2 and the array holds 2
//!   (4), and the string `sh<i>` has 2 + digits(i) characters; digits over 0..199 total 10*1 + 90*2 +
//!   100*3 = 490. Total 200*4 + 200*2 + 490 = 1690.
module Conf;

public class Holder
  public owned _s: Set<string?> := new Set<string?>();
  public owned _a: Array<string?> := new Array<string?>();

  public method Fill(i: int)
    var n := new Set<string?>();
    var arr := new Array<string?>();
    arr.Add("sh" + i.ToString());
    n.Add(arr[0]);
    n.Add(null);
    arr.Add(null);
    _s := n;
    _a := arr;
  end Fill;

  public method Cnt(): int
    return _s.Count + _a.Count;
  end Cnt;

  public method First(): string?
    return _a[0];
  end First;
end Holder;

public class Program
  public static method Main()
    var tot := 0;
    var h := new Holder();
    for var i in 0 ..< 200 do
      h.Fill(i);
      tot := tot + h.Cnt();
      var f := h.First();
      if f <> null then
        tot := tot + f!.Length;
      end if;
    end for;
    Console.WriteLine(tot);
  end Main;
end Program;
