//! case: s28_4_0002
//! doc: spec
//! section: 28.4
//! title: destructors run in subclass-to-base order across three levels
//! rule: Destructors run in subclass-to-base order.
//! expect: run
//! output: child disposed
//!   parent disposed
//!   grandparent disposed
module Conf;

public class GrandParent
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("grandparent disposed");
  end destructor;
end GrandParent;

public class Parent: GrandParent
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("parent disposed");
  end destructor;
end Parent;

public class Child: Parent
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("child disposed");
  end destructor;
end Child;

public class Program
  public static method Main()
    using var c := new Child();
  end Main;
end Program;
