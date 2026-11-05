//! case: s27_6_2219
//! doc: spec
//! section: 27.6
//! title: a receiver-qualified reference to extension members with
//!   identical substituted heads is ambiguous
//! rule: If neither candidate's substituted head is a proper subtype of the
//!   other's, the candidates are equally specific and rule 4 of
//! cites: 27.6, 26.5
//! expect: reject
//! diagnostic: ambiguous
//! notes: Exactly 1 error on the staging tip, all the quoted one; the
//!   accepting twin (the reference replaced by a call, or the
//!   ambiguity removed) compiles.
module Conf;

extension Array<int>
  public method Len(): int => 1;
end;
extension IEnumerable<int>
  public method Len(x: int): int => x + 200;
end;
extension IEnumerable<T>
  public method Len(x: int): int => x + 300;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var g: (int) -> int := a.Len;
    Console.WriteLine(g(5));
  end Main;
end Program;
