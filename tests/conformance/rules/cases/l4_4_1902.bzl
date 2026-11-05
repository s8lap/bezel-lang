//! case: l4_4_1902
//! doc: stdlib
//! section: 4.4
//! title: a user record named Guid that declares IComparable<Guid> sorts through its own CompareTo
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 14.2, 2.3
//! expect: run
//! stdout: 1
module Conf;

record Guid(N: int): IComparable<Guid>
  public method CompareTo(other: Guid): int
    return N - other.N;
  end CompareTo;
end Guid;

class Program
  public static method Main()
    var a: Array<Guid> := [new Guid(2), new Guid(1)];
    a.Sort();
    Console.WriteLine(a[0].N);
  end Main;
end Program;
