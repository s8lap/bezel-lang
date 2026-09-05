public class OwningLinkedList<T>: IOwningCollection<T>, IOwningCollectionLiteralTarget<OwningLinkedList<T>, T> where T: class
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningLinkedList<T>;   // §2.19 witness
  public constructor();

  public pure readonly property Count: int;
  public pure readonly property FirstNode: LinkedListNode<T>?;       // borrow if non-null
  public pure readonly property LastNode: LinkedListNode<T>?;
  public pure readonly property First: T?;                           // borrow
  public pure readonly property Last: T?;

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
  public method TryPopFirst(out owned item: T?): bool;   // v0.171 (#1336)
  public method TryPopLast(out owned item: T?): bool;
  public method Drain(): IOwningEnumerable<T>;
  // Plus the remaining IOwningCollection<T> operations, elided per §1's marker
  //   convention (v0.195): Add(owned item) — appends at the tail, = AddLast —
  //   and IsReadOnly.
end OwningLinkedList;
