//! case: s22_12_0015
//! doc: spec
//! section: 22.12
//! title: each distinct instantiation of a generic class has its own static state
//! rule: A static member of a generic class is accessed by supplying the
//!   type arguments on the type name in receiver position. Each distinct
//!   instantiation has its own static state.
//! cites: 22.12, 22.13
//! expect: run
//! stdout: 1
module Conf;

public static class Registry<T>
  private static _count: int := 0;

  public static method Register(): int
    _count := _count + 1;
    return _count;
  end Register;
end Registry;

public class Program
  public static method Main()
    Registry<int>.Register();
    Registry<int>.Register();
    Console.WriteLine(Registry<string>.Register());
  end Main;
end Program;
