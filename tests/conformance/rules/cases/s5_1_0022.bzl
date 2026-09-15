//! case: s5_1_0022
//! doc: spec
//! section: 5.1
//! title: a call applicable through two co-equal reference conversions is an ambiguous compile error
//! rule: For overload resolution, reference conversions rank co-equal with the
//!   other implicit conversions: given M(a: Animal) and M(o: Object), the call
//!   M(dog) is ambiguous — upcast explicitly.
//! cites: 5.1, 22.6
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

public class Overloads
  public static method M(a: Animal)
    Console.WriteLine("animal");
  end M;

  public static method M(o: Object)
    Console.WriteLine("object");
  end M;
end Overloads;

public class Program
  public static method Main()
    Overloads.M(new Dog());
  end Main;
end Program;
