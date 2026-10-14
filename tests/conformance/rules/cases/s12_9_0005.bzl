//! case: s12_9_0005
//! doc: spec
//! section: 12.9
//! title: IEnumerable<Dog> is accepted as IEnumerable<Animal> under an explicit list
//! rule: Reference-typed T admits the full covariant chain — `IEnumerable<Dog>` is assignable to `IEnumerable<Animal>` and to `IEnumerable<Object>` because the underlying references compose without conversion.
//! cites: 12.9, 14.5
//! expect: accept
module Conf;

public class Animal end Animal;
public class Dog: Animal end Dog;

public class C
  public method S<V>(x: IEnumerable<V>): int => 10;
end C;

public class Program
  public static method Use(): int
    using var c := new C();
    using var l := new Array<Dog>();
    var e: IEnumerable<Dog> := l;
    Console.WriteLine(c.S<Animal>(e));
    return 0;
  end Use;
end Program;
