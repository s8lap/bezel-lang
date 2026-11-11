//! case: s28_3_4511
//! doc: spec
//! section: 28.3
//! title: a plain-var owned local disposed in one branch only still warns for the
//!   other branch's implicit drop
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
  private static method F(n: int)
    var cur := new Node(0);
    for var i in 1..n do cur := new Node(i); end;
    if n > 2 then cur.Dispose(); end;
  end F;
  public static method Main()
    F(3);
    F(1);
  end Main;
end Program;
