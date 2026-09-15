//! case: s17_4_0002
//! doc: spec
//! section: 17.4
//! title: is not is admitted as the two-token operator
//! rule: "`is not` is a single operator with two-token spelling."
//! cites: 17.4
//! expect: run
//! stdout: True
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

public class Program
  public static method Main()
    using var a := new Animal();
    var r := a is not Dog;
    Console.WriteLine(r);
  end Main;
end Program;
