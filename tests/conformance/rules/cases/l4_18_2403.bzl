//! case: l4_18_2403
//! doc: stdlib
//! section: 4.18
//! title: A SortedSet<Guid> of 300 NewGuid() values iterates strictly ascending and
//!   First sorts before Last
//! rule: A comparison-ordered set: membership by `CompareTo`-equality, iteration in
//!   ascending order.
//! cites: 4.18, 2.6
//! expect: run
//! output: True
//!   True
//!   True
//! notes: Red on base: the saved pre-fix compiler of the #8263 review refused
//!   `.First`/`.Last`/`.FirstKey`/`.LastKey` on a Guid element type with
//!   'requires the element type to implement IComparable<T>', although Guid is
//!   declared IComparable<Guid> (stdlib 2.6). No open issue describes this
//!   compile refusal (#8263 describes the iteration order). Strict ascent holds
//!   for any set of distinct elements whatever the 300 values are; the draw
//!   cannot make a Guid equal one already stored except with negligible
//!   probability (122 random bits, stdlib 2.6), and even then the set would
//!   drop the duplicate and still iterate ascending, with n > 1.
module M;
class C
  public static method Main(): int
    using var s := new SortedSet<Guid>();
    var i := 0;
    while i < 300 do
      s.Add(Guid.NewGuid());
      i += 1;
    end;
    var prev := Guid.Empty;
    var ok := true;
    var n := 0;
    for var g in s do
      if n > 0 and prev.CompareTo(g) >= 0 then ok := false; end;
      prev := g;
      n += 1;
    end;
    Console.WriteLine(ok);
    Console.WriteLine(n > 1);
    var f := s.First;
    var l := s.Last;
    Console.WriteLine(f.CompareTo(l) < 0);
    return 0;
  end Main;
end C;
