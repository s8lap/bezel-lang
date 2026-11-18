//! case: s14_2_7204
//! doc: spec
//! section: 14.2
//! title: a type-parameter bound at an Object substitution admits every
//!   class type
//! rule: At `U := Object` the bound admits `Object` and every class type,
//!   and nothing else
//! cites: 14.2
//! expect: run
//! stdout: 1
module Conf;

public class Num
  public constructor()
  end constructor;
end Num;

public class D<Y>
  public constructor()
  end constructor;

  public method ZY<S>(a: S): int where S: Y => 1;
end D;

public class Program
  public static method Main()
    var d := new D<Object>();
    Console.WriteLine(d.ZY<Num>(new Num()));
  end Main;
end Program;
