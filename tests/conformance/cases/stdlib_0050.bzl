public class Stack<T>: ICollection<T>
  public constructor();
  public method Push(item: T);
  public method Pop(): T;             // raises if empty
  public method Peek(): T;
  public method TryPop(out item: T): bool;
  public method TryPeek(out item: T): bool;
end Stack;

public class OwningStack<T>: IOwningCollection<T> where T: class
  public constructor();
  public method Push(owned item: T);
  public method Pop(): owned T;                   // raises if empty; transfers out
  public method Peek(): T;                         // borrow of top element
  public method TryPop(out owned item: T): bool;
  public method TryPeek(out item: T): bool;       // out is borrow
  public method Clear();                           // disposes every element
  public method Drain(): IOwningEnumerable<T>;
end OwningStack;

public class Queue<T>: ICollection<T>
  public constructor();
  public method Enqueue(item: T);
  public method Dequeue(): T;
  public method Peek(): T;
  public method TryDequeue(out item: T): bool;
  public method TryPeek(out item: T): bool;
end Queue;

public class OwningQueue<T>: IOwningCollection<T> where T: class
  public constructor();
  public method Enqueue(owned item: T);
  public method Dequeue(): owned T;
  public method Peek(): T;                         // borrow of front element
  public method TryDequeue(out owned item: T): bool;
  public method TryPeek(out item: T): bool;       // out is borrow
  public method Clear();                           // disposes every element
  public method Drain(): IOwningEnumerable<T>;
end OwningQueue;
