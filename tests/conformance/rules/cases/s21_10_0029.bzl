//! case: s21_10_0029
//! doc: spec
//! section: 21.10
//! title: a local record declared in a lambda do-body implements an interface and is used there
//! rule: `LocalTypeDecl` is admitted wherever a `Statement` is: method,
//!   constructor, destructor, and accessor bodies, lambda `do`-bodies, `do …
//!   end` blocks, loop bodies, and statement-position arm bodies.
//! cites: 21.10, 9.6, 27.3
//! expect: run
//! output: 8
//!   20
module Conf;

public interface IShape
  method Area(): int;
end IShape;

public class Program
  public static method Main()
    var f := lambda(k: int) => do
      record W(N: int): IShape
        public method Area(): int => N * 2;
      end W;
      var s: IShape := new W(k);
      return s.Area();
    end;
    Console.WriteLine(f(4));
    Console.WriteLine(f(10));
  end Main;
end Program;
