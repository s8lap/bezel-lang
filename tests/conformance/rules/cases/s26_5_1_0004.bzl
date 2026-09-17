//! case: s26_5_1_0004
//! doc: spec
//! section: 26.5.1
//! title: distinct names remove an identical-substituted-heads ambiguity
//! rule: Candidates with identical substituted heads never auto-rank and
//!   are rejected as ambiguous (rule 4, §26.5); the recorded remedy for
//!   colliding blocks under the author's control is distinct names, which
//!   removes the collision entirely.
//! cites: 26.5.1, 26.5
//! expect: run
//! stdout: 1
module Conf;

extension IEnumerable<T> where T: value
  public method Head(): T
    for var item in this do return item; end;
    throw new InvalidOperationException("empty");
  end Head;
end;

extension IEnumerable<int>
  public method HeadAsInt(): int
    for var item in this do return item; end;
    throw new InvalidOperationException("empty");
  end HeadAsInt;
end;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    Console.WriteLine(xs.Head());
  end Main;
end Program;
