//! case: s25_2_1212
//! doc: spec
//! section: 25.2
//! title:a user record declaring int times V resolves 3 * v on the right operand's type
//! rule:a binary overload may pair the declaring type with any other type in either position
//! expect: run
//! stdout: 12
module Conf;

public record V(X: int)
  public static method operator *(k: int, v: V): V
    return new V(k * v.X);
  end operator *;
end V;

public class Program
  public static method Main()
    var r := 3 * new V(4);
    Console.WriteLine(r.X);
  end Main;
end Program;
