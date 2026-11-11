//! case: s28_3_4510
//! doc: spec
//! section: 28.3
//! title: a plain-var owned local returned on one path only still warns for the other
//!   path's implicit drop
//! rule: At the implicit-drop site of a plain-`var` owned local
//! cites: 28.3.10
//! expect: warn
//! diagnostic: must-use
//! issue: 8392
module Conf;

public class Node(id: int)
  public property Id: int => id;
  destructor
    Console.WriteLine($"bye {id}");
  end destructor;
end Node;

public class Program
  private static method Build(n: int): owned Node?
    var cur := new Node(0);
    for var i in 1..n do cur := new Node(i); end;
    if n > 5 then return cur; end;
    return null;
  end Build;
  public static method Main()
    using var x := Build(3);
  end Main;
end Program;
