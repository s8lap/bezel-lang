//! case: s26_5_1_0003
//! doc: spec
//! section: 26.5.1
//! title: identical substituted heads never auto-rank, even with a where
//!   clause on one side
//! rule: Candidates with identical substituted heads never auto-rank:
//!   neither a more-concrete-looking declared head nor a stronger `where`
//!   clause on the same head makes one candidate more specific than the
//!   other. A `where` clause is an applicability filter, never a ranking
//!   signal. `extension IEnumerable<T> where T: value` and `extension
//!   IEnumerable<int>` both substitute to `IEnumerable<int>` for an
//!   `Array<int>` receiver.
//! cites: 26.5.1
//! expect: reject
//! diagnostic: Head
module Conf;

extension IEnumerable<T> where T: value
  public method Head(): T
    for var item in this do return item; end;
    throw new InvalidOperationException("empty");
  end Head;
end;

extension IEnumerable<int>
  public method Head(): int
    for var item in this do return item; end;
    throw new InvalidOperationException("empty");
  end Head;
end;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    var h := xs.Head();
  end Main;
end Program;
