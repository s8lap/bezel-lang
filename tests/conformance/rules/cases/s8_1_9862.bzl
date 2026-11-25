//! case: s8_1_9862
//! doc: spec
//! section: 8.1
//! title: A class type name spelling of ReferenceEquals with one argument is rejected
//! rule: public static method ReferenceEquals(a: Object?, b: Object?): bool;
//! cites: 8.1, 22.6
//! expect: reject
//! diagnostic: ReferenceEquals
//! issue: 9021
//! notes: Harvested from the #9021 review (r04_class_arity1). 11.7 has the root's static inherited by every class, so `K.ReferenceEquals` names the same two-parameter form; one argument binds none (8.1, 22.6).
module Conf;
public class K end K;

public class Program
  public static method Main(): int
    var k := new K();
    Console.WriteLine(K.ReferenceEquals(k));
    return 0;
  end Main;
end Program;
