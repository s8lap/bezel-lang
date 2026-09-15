//! case: s29_8_0004
//! doc: spec
//! section: 29.8
//! title: multiple undischarged owned iterator parameters are disposed in reverse declaration order
//! rule: The order across multiple owned parameters is reverse declaration
//!   order, mirroring §28.4's destructor rule.
//! cites: 29.8, 28.4
//! expect: run
//! stdout: second
module Conf;

public class Tag
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    Console.WriteLine(_name);
  end destructor;
end Tag;

public class Program
  public static method Pair(owned first: Tag, owned second: Tag): owned IEnumerable<int>
    yield return 1;
  end Pair;

  public static method Main()
    for var x in Pair(new Tag("first"), new Tag("second")) do
      _ := x;
    end;
  end Main;
end Program;
