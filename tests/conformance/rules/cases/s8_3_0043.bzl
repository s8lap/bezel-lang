//! case: s8_3_0043
//! doc: spec
//! section: 8.3
//! title: an owning choice's plain own-typed Equals obliges no GetHashCode
//! rule: A choice that no boxing reaches, one owning at every
//!   instantiation, has no slot to fill, so its declared `Equals` carries
//!   no such obligation (v0.338, §8.3).
//! cites: 8.3, 35.4
//! expect: run
//! output: ok
//! issue: 5955
module Conf;
class R public V: int := 0; destructor end destructor; end R;
choice FetchOutcome Fetched(owned response: R), NotModified;
  public method Equals(o: FetchOutcome): bool => true;
end FetchOutcome;
class Program public static method Main(): int Console.WriteLine("ok"); return 0; end Main; end Program;
