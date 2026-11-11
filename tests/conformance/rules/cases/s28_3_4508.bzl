//! case: s28_3_4508
//! doc: spec
//! section: 28.3
//! title: a plain-var owned local reassigned in a loop and explicitly disposed
//!   afterwards draws no warning
//! rule: 2. it is explicitly disposed or discarded
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
  private static method F(n: int)
    var cur := new Node(0);
    for var i in 1..n do cur := new Node(i); end;
    cur.Dispose();
  end F;
  public static method Exercise()
    F(3);
  end Exercise;
end Program;
