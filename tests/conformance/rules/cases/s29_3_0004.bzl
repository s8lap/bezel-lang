//! case: s29_3_0004
//! doc: spec
//! section: 29.3
//! title: an iterator method with a plain parameter is admitted
//! rule: An iterator method may not declare `ref` or `out` parameters;
//!   `owned` parameters remain admitted (§29.8) — a plain parameter carries
//!   no such restriction.
//! cites: 29.3
//! expect: accept
module Conf;

public class Program
  public static method Count(n: int): IEnumerable<int>
    yield return n;
  end Count;
end Program;
