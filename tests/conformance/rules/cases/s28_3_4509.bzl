//! case: s28_3_4509
//! doc: spec
//! section: 28.3
//! title: a plain-var owned local reassigned in a loop and moved afterwards draws no
//!   must-use warning
//! rule: 3. it is moved or returned (no implicit drop occurs);
//! cites: 28.3.10, 28.8.1
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
  private static method Take(owned n: Node)
    n.Dispose();
  end Take;
  private static method F(n: int)
    var cur := new Node(0);
    for var i in 1..n do cur := new Node(i); end;
    Take(move cur);
  end F;
  public static method Exercise()
    F(3);
  end Exercise;
end Program;
