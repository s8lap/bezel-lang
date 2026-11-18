//! case: s14_2_7205
//! doc: spec
//! section: 14.2
//! title: a type-parameter bound at a nullable substitution admits no type
//! rule: at a nullable substitution for `U` it admits no type
//! cites: 14.2, 7.1
//! expect: reject
//! diagnostic: type argument
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
    var d := new D<Num?>();
    Console.WriteLine(d.ZY<Num>(new Num()));
  end Main;
end Program;
