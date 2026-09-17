//! case: l4_11_0005
//! doc: stdlib
//! section: 4.11
//! title: a Queue<T> literal places the first element at the front
//! rule: Queue<T>/OwningQueue<T>: the first literal element is at the
//!   front of the queue (the next to be dequeued). [1, 2, 3] dequeues
//!   1, then 2, then 3.
//! cites: 4.11, 15.4
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
