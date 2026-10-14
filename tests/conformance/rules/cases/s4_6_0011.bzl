//! case: s4_6_0011
//! doc: spec
//! section: 4.6
//! title: a string is accepted where IEnumerable<char> is expected
//! rule: `string` implements `IEnumerable<char>`, so strings may be iterated with `for`:
//! cites: 4.6, 5.1
//! expect: accept
module Conf;

public class C
  public method S<V>(x: IEnumerable<V>): int => 10;
end C;

public class Program
  public static method Use(): int
    using var c := new C();
    Console.WriteLine(c.S<char>("abc"));
    return 0;
  end Use;
end Program;
