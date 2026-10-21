//! case: s25_2_0019
//! doc: spec
//! section: 25.2
//! title: a binary operator rejects a right operand that does not implement the parameter's interface
//! rule: "nor an overload from the candidate set above applies is a compile error at the expression"
//!   ([§25.2]). A `StringBuilder` operand does not implement `IA`, so it does not convert to the
//!   parameter.
//! cites: 25.2, 5.1
//! expect: reject
//! diagnostic: applicable operator
//! issue: 6217
module Conf;

public interface IA
end IA;

public class Animal: IA
end Animal;

public class Dog: Animal
end Dog;

public record V(X: int)
  public static method operator +(v: V, p: IA): V => new V(v.X + 1000);
end V;

public class Program
  public static method Main()
    var v := new V(1);
    using var s := new StringBuilder();
    Console.WriteLine((v + s).X);
  end Main;
end Program;
