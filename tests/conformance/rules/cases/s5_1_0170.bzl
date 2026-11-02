//! case: s5_1_0170
//! doc: spec
//! section: 5.1
//! title: the root `Object` still converts into a bare type parameter
//! rule: A value of one type implicitly converts to another when no programmer-visible information is lost — or, for the floating-point cases noted below, when the operation is universally safe enough that explicit casts would create unjustified noise.
//! cites: 5.1, 8.1
//! expect: run
//! output: System.Object
//! issue: 7997
module Conf;

public class Program
  public static method Main(): int
    var r: Object := M<Object>();
    Console.WriteLine(r.ToString());
    return 0;
  end Main;

  public static method M<T>(): owned T => new Object();
end Program;
