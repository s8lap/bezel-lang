//! case: s14_2_0030
//! doc: spec
//! section: 14.2
//! title: where T hashable is not satisfied by a function type
//! rule: A function type is nonetheless not **hashable** (§3.6, clause (e))
//!   ... a `Set<(int) -> int>`, a `Dictionary<(int) -> int, V>`, stdlib §5's
//!   hashable block and `where T: hashable` at a function type are rejected
//!   at the instantiation site
//! cites: 14.2, 3.6, 27.7.1
//! expect: reject
//! diagnostic: hashable
//! issue: 5827
module Conf;

public class Hold<T> where T: hashable
  public constructor()
  end constructor;
end Hold;

public class User
  public static method Run(h: Hold<(int) -> int>): int => 1;
end User;
