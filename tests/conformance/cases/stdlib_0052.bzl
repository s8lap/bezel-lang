public class LinkedList<T>: ICollection<T>, ICollectionLiteralTarget<LinkedList<T>, T>
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned LinkedList<T>;   // §2.19 witness (spec §15.4)
  public constructor();
  public constructor(source: IEnumerable<T>);

  public pure readonly property Count: int;
  public pure readonly property FirstNode: LinkedListNode<T>?;
  public pure readonly property LastNode: LinkedListNode<T>?;
  public pure readonly property First: T?;
  public pure readonly property Last: T?;

  public method AddFirst(item: T): LinkedListNode<T>;
  public method AddLast(item: T): LinkedListNode<T>;
  public method AddBefore(node: LinkedListNode<T>, item: T): LinkedListNode<T>;
  public method AddAfter(node: LinkedListNode<T>, item: T): LinkedListNode<T>;
  public method RemoveFirst();
  public method RemoveLast();
  public method RemoveNode(node: LinkedListNode<T>);
  public method Find(item: T): LinkedListNode<T>?;
  // Plus the standard ICollection<T> operations: Add (appends at the tail,
  // = AddLast — v0.195), Clear, Contains, Remove, IsReadOnly.
end LinkedList;

public class LinkedListNode<T>
  public readonly property Value: T;
  public readonly property Next: LinkedListNode<T>?;
  public readonly property Previous: LinkedListNode<T>?;
end LinkedListNode;
