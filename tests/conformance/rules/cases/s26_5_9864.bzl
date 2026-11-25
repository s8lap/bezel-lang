//! case: s26_5_9864
//! doc: spec
//! section: 26.5
//! title: An alias of a record does not supply the ReferenceEquals static
//! rule: a call or read `T.M` that none of them supplies names no member and is the same compile error
//! cites: 26.5, 8.1, 11.2, 27.8
//! expect: reject
//! diagnostic: no member
//! issue: 9021
//! notes: Harvested from the #9021 review (r12_alias_record). An alias sees through to the aliased type (27.8), so it supplies exactly what the record's type name supplies, which is no ReferenceEquals.
module Conf;
public record R(X: int);
type RA := R;

public class Program
  public static method Main(): int
    var r := new R(1);
    Console.WriteLine(RA.ReferenceEquals(r, r));
    return 0;
  end Main;
end Program;
