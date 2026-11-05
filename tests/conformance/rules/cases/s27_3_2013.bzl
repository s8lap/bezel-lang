//! case: s27_3_2013
//! doc: spec
//! section: 27.3
//! title: a lambda returning a class value at an interface slot the class
//!   implements is admitted
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1
//! expect: run
//! stdout: 5
module Conf;

public interface IHasN
  method N(): int;
end IHasN;

class Nn: IHasN
  public method N(): int => 5;
end Nn;

public class Program
  public static method Main()
    using var n := new Nn();
    var f: (Nn) -> IHasN := lambda(x) => x;
    Console.WriteLine(f(n).N());
  end Main;
end Program;
