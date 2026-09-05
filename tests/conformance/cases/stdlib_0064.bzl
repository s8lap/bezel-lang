public class Stack<T>: IReadOnlyCollection<T>, ICollectionLiteralTarget<Stack<T>, T>   // v0.165 (#1354)
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Stack<T>;   // §2.19 witness; first element
                                                                                                //   pushed first (spec §15.4)
  public constructor();
  public method Push(item: T);
  public method Contains(item: T): bool;        // explicit (v0.165; declaration restored v0.195)
  public method Pop(): T;             // raises if empty
  public pure method Peek(): T;
  public method TryPop(out item: T?): bool;    // v0.171 (#1336)
  public method TryPeek(out item: T?): bool;
  public method Clear();                        // v0.222 (#1811): drops all elements; disposes nothing (§28.6)
end Stack;

public class OwningStack<T>: IReadOnlyCollection<T>, IOwningCollectionLiteralTarget<OwningStack<T>, T> where T: class   // v0.222 (#1811): demoted, completing v0.165 #1354
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningStack<T>;   // §2.19 witness
  public constructor();
  public method Push(owned item: T);
  public method Contains(item: T): bool;        // explicit (v0.222, #1811 — the v0.165 pattern)
  public method Pop(): owned T;                   // raises if empty; transfers out
  public pure method Peek(): T;                         // borrow of top element
  public method TryPop(out owned item: T?): bool;   // v0.171 (#1336)
  public method TryPeek(out item: T?): bool;       // out is borrow
  public method Clear();                           // disposes every element
  public method Drain(): IOwningEnumerable<T>;
end OwningStack;

public class Queue<T>: IReadOnlyCollection<T>, ICollectionLiteralTarget<Queue<T>, T>   // v0.165 (#1354)
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Queue<T>;   // §2.19 witness; first element
                                                                                                //   at the front (spec §15.4)
  public constructor();
  public method Enqueue(item: T);
  public method Dequeue(): T;
  public pure method Peek(): T;
  public method Contains(item: T): bool;        // explicit (v0.165)
  public method TryDequeue(out item: T?): bool;   // v0.171 (#1336)
  public method TryPeek(out item: T?): bool;
  public method Clear();                        // v0.222 (#1811): drops all elements; disposes nothing (§28.6)
end Queue;

public class OwningQueue<T>: IReadOnlyCollection<T>, IOwningCollectionLiteralTarget<OwningQueue<T>, T> where T: class   // v0.222 (#1811)
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningQueue<T>;   // §2.19 witness
  public constructor();
  public method Enqueue(owned item: T);
  public method Contains(item: T): bool;        // explicit (v0.222, #1811 — the v0.165 pattern)
  public method Dequeue(): owned T;
  public pure method Peek(): T;                         // borrow of front element
  public method TryDequeue(out owned item: T?): bool;   // v0.171 (#1336)
  public method TryPeek(out item: T?): bool;       // out is borrow
  public method Clear();                           // disposes every element
  public method Drain(): IOwningEnumerable<T>;
end OwningQueue;
