//! case: l4_18_2401
//! doc: stdlib
//! section: 4.18
//! title: SortedSet<Guid> iterates ascending by Guid.CompareTo and drops an equal
//!   element
//! rule: A comparison-ordered set: membership by `CompareTo`-equality, iteration in
//!   ascending order.
//! cites: 4.18, 2.6
//! expect: run
//! output: 00000000-0000-0000-0000-000000000000
//!   00000000-0000-0000-0000-00000000007f
//!   00000000-0000-0000-0000-000000000080
//!   00000000-0000-0000-0000-0000000000ff
//!   80000000-0000-0000-0000-000000000000
//!   ff000000-0000-0000-0000-000000000000
//!   6
//! issue: 8263
//! notes: Red on base: the saved pre-fix compiler of the #8263 review iterated
//!   SortedSet<Guid> and SortedDictionary<Guid, V> in insertion order (the
//!   defect #8263 describes). Guid.CompareTo is lexicographic over the
//!   big-endian bytes (2.6), so 0x7f < 0x80 < 0xff in the last byte and 0x00 <
//!   0x80 < 0xff in the first. Guid.Empty is the all-zero Guid, equal to the
//!   parsed all-zero one, so the second add leaves the set unmodified (4.18)
//!   and seven adds give six elements. Canonical D rendering (2.6).
module M;
class C
  public static method Main(): int
    using var s := new SortedSet<Guid>();
    s.Add(Guid.Parse("ff000000-0000-0000-0000-000000000000"));
    s.Add(Guid.Parse("80000000-0000-0000-0000-000000000000"));
    s.Add(Guid.Parse("00000000-0000-0000-0000-000000000000"));
    s.Add(Guid.Empty);
    s.Add(Guid.Parse("00000000-0000-0000-0000-0000000000ff"));
    s.Add(Guid.Parse("00000000-0000-0000-0000-000000000080"));
    s.Add(Guid.Parse("00000000-0000-0000-0000-00000000007f"));
    for var g in s do Console.WriteLine(g); end;
    Console.WriteLine(s.Count);
    return 0;
  end Main;
end C;
