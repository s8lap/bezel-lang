//! case: s28_4_0002
//! doc: spec
//! section: 28.4
//! title: destructors run in subclass-to-base order across three levels
//! rule: Destructors run in subclass-to-base order.
//! expect: run
//! stdout: child disposed
module Conf;

public class GrandParent
  destructor
    Console.WriteLine("grandparent disposed");
  end destructor;
end GrandParent;

public class Parent: GrandParent
  destructor
    Console.WriteLine("parent disposed");
  end destructor;
end Parent;

public class Child: Parent
  destructor
    Console.WriteLine("child disposed");
  end destructor;
end Child;

public class Program
  public static method Main()
    using var c := new Child();
  end Main;
end Program;
