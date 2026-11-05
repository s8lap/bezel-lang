//! case: l2_6_2401
//! doc: stdlib
//! section: 2.6
//! title: SortedDictionary<Guid, string> yields values in key order after
//!   Array<Guid>.Sort()
//! rule: CompareTo = lexicographic over the 16 canonical big-endian
//! cites: 2.6, 4.17, 4.18, 4.4
//! expect: run
//! output: 00000000-0000-0000-0000-000000000001
//!   22222222-2222-2222-2222-222222222222
//!   f0000000-0000-0000-0000-000000000000
//!   a
//!   c
//! issue: 8263
//! notes: Red on base: the saved pre-fix compiler of the #8263 review printed the
//!   dictionary values in insertion order, c then a (SortedDictionary<Guid, V>
//!   ignored Guid.CompareTo, the defect #8263 describes). After Sort() arr is
//!   ascending, so arr[0] is the all-zero-prefixed Guid and arr[2] the f0 one;
//!   the dictionary yields the value of the lesser key first. The SortedSet
//!   half is green on base because its input is already ascending.
module M;
class C
  public static method Main(): int
    var arr := new Array<Guid>();
    arr.Add(Guid.Parse("22222222-2222-2222-2222-222222222222"));
    arr.Add(Guid.Parse("00000000-0000-0000-0000-000000000001"));
    arr.Add(Guid.Parse("f0000000-0000-0000-0000-000000000000"));
    arr.Sort();
    using var s := new SortedSet<Guid>();
    for var g in arr do s.Add(g); end;
    for var g in s do Console.WriteLine(g); end;
    using var d := new SortedDictionary<Guid, string>();
    d.Add(arr[2], "c");
    d.Add(arr[0], "a");
    for var kv in d do Console.WriteLine(kv.Value); end;
    return 0;
  end Main;
end C;
