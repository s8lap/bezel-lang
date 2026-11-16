//! case: s7_5_4803
//! doc: spec
//! section: 7.5
//! title: a null-conditional Queue and Stack Enqueue, Push and Clear perform on non-null and skip on null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. ... The receiver is evaluated first; if it is null the access does not occur and nothing to its right in the chain is evaluated
//! cites: 7.5, stdlib 4
//! expect: run
//! output: 3
//!   0
//!   3
//!   2
//!   0
//!   2
//! issue: 8501
module Conf;

public class Counter
  public static calls: int := 0;
  public static method Next(): int
    calls := calls + 1;
    return 3;
  end Next;
end Counter;

public class Program
  public static method Main()
    using var q: Queue<int>? := [1, 2];
    q?.Enqueue(Counter.Next());
    Console.WriteLine(q!.Count);
    q?.Clear();
    Console.WriteLine(q!.Count);
    using var k: Stack<int>? := [1, 2];
    k?.Push(Counter.Next());
    Console.WriteLine(k!.Pop());
    Console.WriteLine(k!.Count);
    k?.Clear();
    Console.WriteLine(k!.Count);
    using var qn: Queue<int>? := null;
    using var kn: Stack<int>? := null;
    qn?.Enqueue(Counter.Next());
    kn?.Push(Counter.Next());
    Console.WriteLine(Counter.calls);
  end Main;
end Program;
