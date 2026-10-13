//! case: s3_6_0046
//! doc: spec
//! section: 3.6
//! title: a Set of records with a declared Equals and GetHashCode holds one element for two keys
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
  var s := new Set<Key>(); s.Add(new Key(1)); s.Add(new Key(2)); Console.WriteLine(s.Count.ToString());
  return 0;
end Main; end C;
