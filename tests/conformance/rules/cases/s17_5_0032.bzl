//! case: s17_5_0032
//! doc: spec
//! section: 17.5
//! title: an int left operand widens to long for membership in a Set<long>
//! rule: "A value of one type implicitly converts to another when no
//!   programmer-visible information is lost" — `int` converts to `long` in the
//!   §5.1 table — and the left operand is admitted exactly when admissible as
//!   `Contains(item: long)`'s argument ([§17.5]). The element 2 is present, so
//!   the membership test is True.
//! cites: 17.5, 5.1
//! expect: run
//! stdout: True
//! issue: 6625
module Conf;

public class Program
  public static method Main()
    var s: Set<long> := [1, 2];
    var i: int := 2;
    var found := i in s;
    Console.WriteLine(found);
  end Main;
end Program;
