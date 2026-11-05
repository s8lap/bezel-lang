//! case: l2_6_1906
//! doc: stdlib
//! section: 2.6
//! title: Sort and Sorted over 2000 random Guids agree with CompareTo and with the canonical text order
//! rule: CompareTo = lexicographic over the 16 canonical big-endian
//! cites: 4.4, 14.2
//! expect: run
//! stdout: 0
//! issue: 8237
module Conf;

class Program
  public static method SortIt(a: Array<Guid>)
    a.Sort();
  end SortIt;

  public static method Main()
    var a: Array<Guid> := [];
    for var i in 0..<2000 do a.Add(Guid.NewGuid()); end;
    var s := a.Sorted();
    SortIt(a);
    var bad := 0;
    for var i in 1..<a.Count do
      if a[i - 1].CompareTo(a[i]) > 0 then bad := bad + 1; end;
      if a[i - 1].ToString() > a[i].ToString() then bad := bad + 1000; end;
      if s[i].ToString() <> a[i].ToString() then bad := bad + 100000; end;
    end;
    Console.WriteLine(bad);
  end Main;
end Program;
