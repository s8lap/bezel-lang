//! case: s29_3_0003
//! doc: spec
//! section: 29.3
//! title: an iterator method may not declare a ref parameter
//! rule: An iterator method may not declare `ref` or `out` parameters: its
//!   arguments are captured into a heap state-machine frame that outlives
//!   the synchronous call, and a caller-stack-slot reference cannot be.
//! cites: 29.3, 30.1
//! expect: reject
//! diagnostic: ref
module Conf;

public class Program
  public static method Count(ref n: int): IEnumerable<int>
    yield return n;
  end Count;
end Program;
