//! case: s26_5_2408
//! doc: spec
//! section: 26.5
//! title: A user extension IEnumerable<T> Min() does not collide with the stdlib
//!   block (filter leaves one)
//! rule: A `where` clause is an *applicability* filter, never a ranking signal.
//! cites: 26.5, 26.5.1
//! expect: run
//! output: 4242
//! notes: Red on base: the saved pre-fix compiler reported 'ambiguous extension
//!   member Min: 2 extensions are equally specific', counting the stdlib
//!   candidate although Array<int> is not IComparable<Array<int>> and the where
//!   clause is an applicability filter, never a ranking signal. Both heads are
//!   IEnumerable<Array<int>> after substitution, so the call is unambiguous
//!   only because the filter removes the stdlib candidate. It passes on the
//!   staging tip. #8199 describes the Object-extension variant of the same
//!   fault (the stdlib candidate is not filtered) with the #2770 guard text,
//!   not this ambiguity error.
module Conf;

extension IEnumerable<T>
  public method Min(): int => 4242;
end;

public class Program
  public static method Main(): int
    using var a := new Array<Array<int>>();
    Console.WriteLine(a.Min());
    return 0;
  end Main;
end Program;
