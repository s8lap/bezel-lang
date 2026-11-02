//! case: s26_5_0063
//! doc: spec
//! section: 26.5
//! title: a concrete and a generic extension head with identical substituted heads are ambiguous
//! rule: In particular, candidates with **identical substituted heads** never
//!   auto-rank: neither a more-concrete-looking declared head
//!   (`extension IEnumerable<int>` and `extension IEnumerable<T>` are
//!   indistinguishable after substitution for an `Array<int>` receiver) nor a
//!   stronger `where` clause on the same head makes one candidate more specific
//!   than the other.
//! cites: 26.5.1, 26.5
//! expect: reject
//! diagnostic: candidates
module Conf;

extension IEnumerable<int>
  public method Tag(n: int): int => n;
end extension;

extension IEnumerable<T>
  public method Tag(n: T): T => n;
end extension;

public class Program
  public static method Main(): int
    var a: Array<int> := [1, 2];
    Console.WriteLine(a.Tag(5).ToString());
    return 0;
  end Main;
end Program;
