//! case: s11_6_0022
//! doc: spec
//! section: 11.6
//! title: a chain head supplying the enclosing type's arguments reaches a nested type's static member
//! rule: A chain head that supplies the enclosing type's arguments (§2.7 rule 1, `Tree<int>.Node<int>
//!   .Create()`) reaches that one type whatever the arguments, and its static members have one
//!   cell each (§22.12), not one per enclosing instantiation (§11.6, v0.340).
//! cites: 11.6, 2.7
//! expect: run
//! stdout: 7
module Conf;

public class Tree<T>
  public class Node
    public static method Create(): int => 7;
  end Node;
end Tree;

public class Program
  public static method Main(): int
    Console.WriteLine(Tree<int>.Node.Create());
    return 0;
  end Main;
end Program;
