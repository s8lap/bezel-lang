public class OwningLinkedList<T>: IOwningCollection<T> where T: class
  public constructor();

  public readonly property Count: int;
  public readonly property FirstNode: LinkedListNode<T>?;       // borrow if non-null
  public readonly property LastNode: LinkedListNode<T>?;
  public readonly property First: T?;                           // borrow
  public readonly property Last: T?;

  public method AddFirst(owned item: T): LinkedListNode<T>;
  public method AddLast(owned item: T): LinkedListNode<T>;
  public method AddBefore(node: LinkedListNode<T>, owned item: T): LinkedListNode<T>;
  public method AddAfter(node: LinkedListNode<T>, owned item: T): LinkedListNode<T>;
  public method RemoveFirst();                                  // disposes
  public method RemoveLast();                                   // disposes
  public method RemoveNode(node: LinkedListNode<T>);            // disposes
  public method Clear();                                         // disposes every element
  public method Contains(item: T): bool;                         // borrow-taking
  public method Find(item: T): LinkedListNode<T>?;               // borrow-taking
  public method Remove(item: T): bool;                           // disposes the matched

  // Drain methods
  public method PopFirst(): owned T;                            // raises if empty
  public method PopLast(): owned T;
  public method TryPopFirst(out owned item: T): bool;
  public method TryPopLast(out owned item: T): bool;
  public method Drain(): IOwningEnumerable<T>;
end OwningLinkedList;
