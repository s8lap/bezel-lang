//! case: l4_17_2401
//! doc: stdlib
//! section: 4.17
//! title: SortedDictionary<Guid, int> iterates ascending by key; FirstKey, LastKey
//!   and Remove agree
//! rule: A comparison-ordered associative collection: iteration visits entries in
//!   ascending key order.
//! cites: 4.17, 2.6
//! expect: run
//! output: 11111111-1111-1111-1111-1111111111ff
//!   a1111111-1111-1111-1111-111111111111
//!   11111111-1111-1111-1111-1111111111ff
//!   0
//!   22222222-2222-2222-2222-222222222222
//!   2
//!   a1111111-1111-1111-1111-111111111111
//!   1
//!   True
//!   11111111-1111-1111-1111-1111111111ff
//!   a1111111-1111-1111-1111-111111111111
//! issue: 8263
//! notes: Red on base: the saved pre-fix compiler of the #8263 review iterated
//!   SortedSet<Guid> and SortedDictionary<Guid, V> in insertion order (the
//!   defect #8263 describes). It also refused FirstKey/LastKey on the Guid key
//!   (requires IComparable<K>). Keys in ascending order: 0x11.. < 0x22.. <
//!   0xa1.. in the first byte (2.6). The parsed 0x11..ff key differs from the
//!   others in the first byte, so it sorts first.
module M;
class C
  public static method Main(): int
    using var d := new SortedDictionary<Guid, int>();
    d.Add(Guid.Parse("22222222-2222-2222-2222-222222222222"), 2);
    d.Add(Guid.Parse("a1111111-1111-1111-1111-111111111111"), 1);
    d.Add(Guid.Parse("11111111-1111-1111-1111-1111111111ff"), 0);
    Console.WriteLine(d.FirstKey);
    Console.WriteLine(d.LastKey);
    for var kv in d do
      Console.WriteLine(kv.Key);
      Console.WriteLine(kv.Value);
    end;
    var removed := d.Remove(Guid.Parse("22222222-2222-2222-2222-222222222222"));
    Console.WriteLine(removed);
    for var kv in d do Console.WriteLine(kv.Key); end;
    return 0;
  end Main;
end C;
