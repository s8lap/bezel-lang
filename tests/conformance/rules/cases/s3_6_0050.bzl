//! case: s3_6_0050
//! doc: spec
//! section: 3.6
//! title: a record without user equality keeps structural equality in a Set
//! rule: A positional record's default Equals is field-by-field value
//!   equality over its admissible component fields (v0.338, §8.3 default
//!   table).
//! cites: 3.6, 9.2
//! expect: run
//! output: 2
module Conf;
record Key(X: int);
class C public static method Main(): int
  var s := new Set<Key>(); s.Add(new Key(1)); s.Add(new Key(2)); s.Add(new Key(1)); Console.WriteLine(s.Count.ToString());
  return 0;
end Main; end C;
