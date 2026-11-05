//! case: l4_18_2404
//! doc: stdlib
//! section: 4.18
//! title: SortedSet<Guid> orders ascending in each of two modules that both name the
//!   instantiation
//! rule: A comparison-ordered set: membership by `CompareTo`-equality, iteration in
//!   ascending order.
//! cites: 4.18, 2.6
//! expect: run
//! output: 01111111-1111-1111-1111-111111111111
//!   f2222222-2222-2222-2222-222222222222
//!   11111111-1111-1111-1111-111111111111
//! issue: 8263
//! notes: Red on base: the saved pre-fix compiler of the #8263 review iterated
//!   SortedSet<Guid> and SortedDictionary<Guid, V> in insertion order (the
//!   defect #8263 describes). The same instantiation SortedSet<Guid> is named
//!   in both modules; each must order by Guid.CompareTo (here the importing
//!   module iterates ascending and the library's First is the lesser of its two
//!   keys, 0x11.. < 0x22..).
module M;
import Lib;
class C
  public static method Main(): int
    using var s := new SortedSet<Guid>();
    s.Add(Guid.Parse("f2222222-2222-2222-2222-222222222222"));
    s.Add(Guid.Parse("01111111-1111-1111-1111-111111111111"));
    for var g in s do Console.WriteLine(g); end;
    var r := L.F();
    return r;
  end Main;
end C;
