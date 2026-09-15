//! case: s17_4_0007
//! doc: spec
//! section: 17.4
//! title: is and as are non-destructive — the operand keeps its ownership state
//! rule: "`is`, `is not`, and `as` are non-destructive on their operand: ...
//!   after `if x is T then ... end` — and after `var r := x as T` — `x`
//!   retains whatever ownership state it had before the test."
//! cites: 17.4
//! expect: run
//! stdout: found
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
  public static method Acquire(): owned Animal => new Dog();

  public static method Main()
    using var thing: Animal := Acquire();
    var maybeDog := thing as Dog;
    if maybeDog <> null then
      Console.WriteLine("found");
    end;
  end Main;
end Program;
