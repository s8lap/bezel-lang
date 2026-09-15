//! case: s14_5_0001
//! doc: spec
//! section: 14.5
//! title: a partial explicit type-argument list is rejected
//! rule: An explicit type-argument list binds the declared type parameters in
//!   declaration order and must supply exactly as many arguments as the
//!   method declares type parameters; a shorter or longer list is a compile
//!   error — there are no partial lists (§14.5).
//! cites: 14.5
//! expect: reject
//! diagnostic: type argument
module Conf;

public static class Util
  public static method Pick<T, U>(a: T, b: U): T => a;
end Util;

public class Program
  public static method Main()
    var x := Util.Pick<int>(1, "s");
    Console.WriteLine(x);
  end Main;
end Program;
