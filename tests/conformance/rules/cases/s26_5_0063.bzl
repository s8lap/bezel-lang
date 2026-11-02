//! case: s26_5_0063
//! doc: spec
//! section: 26.5
//! title: a concrete and a generic extension head with identical substituted heads are ambiguous
//! rule: If neither candidate's substituted head is a proper subtype of the
//!   other's, the candidates are equally specific and rule 4 of
//!   [§26.5](#sec-resolution) produces an ambiguity error.
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
