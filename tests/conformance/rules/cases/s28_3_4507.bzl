//! case: s28_3_4507
//! doc: spec
//! section: 28.3
//! title: a plain-var owned local reassigned in a loop and returned on every path
//!   draws no must-use warning
//! rule: 3. it is moved or returned (no implicit drop occurs);
//! cites: 28.3.10
//! expect: quiet
//! issue: 8392
module Conf;

public class Node(id: int)
  public property Id: int => id;
  destructor
    Console.WriteLine($"bye {id}");
  end destructor;
end Node;

public class Program
  private static method Build(n: int): owned Node
    var cur := new Node(0);
    for var i in 1..n do cur := new Node(i); end;
    return cur;
  end Build;
  public static method Exercise()
    using var x := Build(3);
  end Exercise;
end Program;
