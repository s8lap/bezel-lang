//! case: s21_10_0026
//! doc: spec
//! section: 21.10
//! title: a local record in a generic method declares its own generic method and calls it at two types
//! rule: The local type is an ordinary nominal record or enum — every §9 (records)
//!   and §10 (enums) rule applies unchanged. The GenericParams? slot must be
//!   empty (v1) — a local type declares no type parameters of its own.
//! cites: 21.10, 14.1
//! expect: run
//! output: 3
//!   a
module Conf;

public static class Util
  public static method Go<T>(t: T)
    record P(A: int)
      public method Pick<U>(x: U, y: U): U => (if A > 0 then x else y);
    end P;
    var p := new P(1);
    Console.WriteLine(p.Pick(3, 4));
    Console.WriteLine(p.Pick("a", "b"));
  end Go;
end Util;

public class Program
  public static method Main()
    Util.Go<long>(5L);
  end Main;
end Program;
