//! case: s7_4_0051
//! doc: spec
//! section: 7.4
//! title: a non-null int declaration initializer narrows a nullable local for a constructor argument
//! rule: "from a declaration whose initializer is such an `expr`" ([§7.4]), and "where `expr`'s static type is non-nullable": the `int` initializer narrows the local, so the constructor's non-nullable `int` parameter admits it.
//! cites: 7.4
//! expect: run
//! stdout: 5
//! issue: 6223
module Conf;

public class A
  public constructor(v: int) _v := v; end constructor;
  public _v: int := 0;
end A;

public class Program
  public static method Main()
    var i: int? := 5;
    var a := new A(i);
    Console.WriteLine(a._v);
  end Main;
end Program;
