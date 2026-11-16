//! case: s7_5_4804
//! doc: spec
//! section: 7.5
//! title: a null-conditional LinkedList RemoveFirst, RemoveLast and Clear perform on non-null, skip on null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. ... The receiver is evaluated first; if it is null the access does not occur and nothing to its right in the chain is evaluated
//! cites: 7.5, stdlib 4
//! expect: run
//! output: [2,3,]
//!   [2,]
//!   []
//!   null ok
//! issue: 8501
module Conf;

public class Program
  public static method Show(l: LinkedList<int>): string
    var s := "";
    for var e in l do
      s := s + $"{e},";
    end;
    return $"[{s}]";
  end Show;

  public static method Main()
    using var l: LinkedList<int>? := [1, 2, 3];
    l?.RemoveFirst();
    Console.WriteLine(Show(l!));
    l?.RemoveLast();
    Console.WriteLine(Show(l!));
    l?.Clear();
    Console.WriteLine(Show(l!));
    using var n: LinkedList<int>? := null;
    n?.RemoveFirst();
    n?.Clear();
    Console.WriteLine("null ok");
  end Main;
end Program;
