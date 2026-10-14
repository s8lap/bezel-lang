//! case: s22_6_0092
//! doc: spec
//! section: 22.6
//! title: a string argument is not applicable to IEnumerable<int> under an explicit list
//! rule: Applicability over the (substituted) signatures: parameter types under the implicit conversions (§5.1) and the `ref`/`out` modifiers.
//! cites: 22.6, 4.6, 5.1
//! expect: reject
//! diagnostic: implicit
module Conf;

public class C
  public method S<V>(x: IEnumerable<V>): int => 10;
end C;

public class Program
  public static method Use(): int
    using var c := new C();
    Console.WriteLine(c.S<int>("abc"));
    return 0;
  end Use;
end Program;
