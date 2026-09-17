//! case: s29_3_0005
//! doc: spec
//! section: 29.3
//! title: an iterator method may not declare an out parameter
//! rule: An iterator method may not declare `ref` or `out` parameters: its
//!   arguments are captured into a heap state-machine frame that outlives
//!   the synchronous call, and a caller-stack-slot reference cannot be.
//! cites: 29.3, 30.1
//! expect: reject
//! diagnostic: out
module Conf;

public class Program
  public static method TryCount(n: int, out ok: bool): IEnumerable<int>
    ok := true;
    yield return n;
  end TryCount;
end Program;
