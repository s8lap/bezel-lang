//! case: s15_4_0014
//! doc: spec
//! section: 15.4
//! title: a Queue<T> literal puts the first element at the front
//! rule: For `Queue<T>` the first literal element is at the front of the queue
//!   (the next to be dequeued); the last literal element is at the back.
//!   `[1, 2, 3]` dequeues 1, then 2, then 3 (§15.4).
//! cites: 15.4
//! expect: run
//! output: 1
//!   2
//!   3
module Conf;

public class Program
  public static method Main()
    using var q: Queue<int> := [1, 2, 3];
    Console.WriteLine(q.Dequeue());
    Console.WriteLine(q.Dequeue());
    Console.WriteLine(q.Dequeue());
  end Main;
end Program;
