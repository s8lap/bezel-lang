//! case: s3_6_0047
//! doc: spec
//! section: 3.6
//! title: a Dictionary keyed by a record with a declared Equals treats the keys as one
//! rule: A value-like choice outside auto-hashable through its own
//!   declared equality, `Equals(other: C)` with `GetHashCode`, is hashable
//!   on the strength of those members (v0.338, §3.6 hashable row).
//! cites: 3.6, 8.3
//! expect: run
//! output: 1
//! issue: 5997
module Conf;
record Key(X: int)
  public method Equals(other: Key): bool => true;
  public method GetHashCode(): int => 0;
  public static method operator =(a: Key, b: Key): bool => true;
  public static method operator <>(a: Key, b: Key): bool => false;
end Key;
class C public static method Main(): int
  var d := new Dictionary<Key, int>(); d.Add(new Key(1), 1); d[new Key(2)] := 2; Console.WriteLine(d.Count.ToString());
  return 0;
end Main; end C;
