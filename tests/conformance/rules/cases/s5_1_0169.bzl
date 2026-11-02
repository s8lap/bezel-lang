//! case: s5_1_0169
//! doc: spec
//! section: 5.1
//! title: a user generic `Object<T>` buys no conversion into a bare type parameter
//! rule: A value of one type implicitly converts to another when no programmer-visible information is lost — or, for the floating-point cases noted below, when the operation is universally safe enough that explicit casts would create unjustified noise.
//! cites: 5.1, 8.1, 2.2
//! expect: reject
//! diagnostic: type parameter
//! issue: 7997
module Conf;

public class Object<T>
  public method Hi(): int => 42;
end Object;

public class Program
  public static method Main(): int
    var x: int := M<int>();
    Console.WriteLine(x);
    using var o := new Object<int>();
    Console.WriteLine(o.Hi());
    return 0;
  end Main;

  public static method M<T>(): owned T => new Object<int>();
end Program;
