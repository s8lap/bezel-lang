//! case: s8_1_9860
//! doc: spec
//! section: 8.1
//! title: Object.ReferenceEquals called with one argument is rejected
//! rule: public static method ReferenceEquals(a: Object?, b: Object?): bool;
//! cites: 8.1, 22.6
//! expect: reject
//! diagnostic: ReferenceEquals
//! issue: 9021
//! notes: Harvested from the #9021 review (r43-rev-m151-9021, r01_arity1). 8.1 declares exactly one form with two parameters, so a one-argument call binds no declared form (22.6). The accepting twin is the two-argument call, covered by the 8.4 and 11.7 ReferenceEquals cases.
module Conf;
public class K end K;

public class Program
  public static method Main(): int
    var k := new K();
    Console.WriteLine(Object.ReferenceEquals(k));
    return 0;
  end Main;
end Program;
