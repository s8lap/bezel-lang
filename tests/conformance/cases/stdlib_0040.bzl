public class LinkedList<T>: ICollection<T>
  public constructor();
  public constructor(source: IEnumerable<T>);

  public readonly property Count: int;
  public readonly property FirstNode: LinkedListNode<T>?;
  public readonly property LastNode: LinkedListNode<T>?;
  public readonly property First: T?;
  public readonly property Last: T?;

  public method AddFirst(item: T): LinkedListNode<T>;
  public method AddLast(item: T): LinkedListNode<T>;
  public method AddBefore(node: LinkedListNode<T>, item: T): LinkedListNode<T>;
  public method AddAfter(node: LinkedListNode<T>, item: T): LinkedListNode<T>;
  public method RemoveFirst();
  public method RemoveLast();
  public method RemoveNode(node: LinkedListNode<T>);
  public method Find(item: T): LinkedListNode<T>?;
  // Plus the standard ICollection<T> operations: Add, Clear, Contains, Remove,
  // IsReadOnly. (§28.11 v0.213, #2081: `DisposeElements()` removed with axiom A8.)
end LinkedList;

public class LinkedListNode<T>
  public readonly property Value: T;
  public readonly property Next: LinkedListNode<T>?;
  public readonly property Previous: LinkedListNode<T>?;
end LinkedListNode;
