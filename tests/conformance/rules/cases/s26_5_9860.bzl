//! case: s26_5_9860
//! doc: spec
//! section: 26.5
//! title: A record type name does not supply the ReferenceEquals static
//! rule: a call or read `T.M` that none of them supplies names no member and is the same compile error
//! cites: 26.5, 8.1, 11.2, 27.8
//! expect: reject
//! diagnostic: no member
//! issue: 9021
//! notes: Harvested from the #9021 review (r06_record). 8.1: a record reaches Object by boxing when used in an Object-typed position, and 11.7 has the static inherited by classes; through the type name R (26.5 rule 2) nothing supplies `ReferenceEquals`, so the access names no member. The spelling that works is `Object.ReferenceEquals(r, r)` (twin s8_4_9861).
module Conf;
public record R(X: int);

public class Program
  public static method Main(): int
    var r := new R(1);
    Console.WriteLine(R.ReferenceEquals(r, r));
    return 0;
  end Main;
end Program;
