// bug: B20
// title: Must-use warning on an owned local that every path returns
// issue: #8392
// rests-on: spec §28.9 (v0.355): `Node` is trivial-dispose, so no must-use diagnostic attaches; every path returns the local (§28.3)
// source: data/compiler-bugs.md, verbatim below this header
// expect: quiet
// stdout: ok
module P;
public class Node
end Node;
public class Program
  private static method Build(n: int): owned Node
    var cur := new Node();
    for var i in 1..n do
      cur := new Node();
    end;
    return cur;
  end Build;
  public static method Main()
    using var x := Build(3);
    Console.WriteLine("ok");
  end Main;
end Program;
