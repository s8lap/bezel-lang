public class LinkedList<T>: ICollection<T>
  constructor();  constructor(source: IEnumerable<T>);
  pure readonly property Count: int;  FirstNode, LastNode: LinkedListNode<T>?;  First, Last: T?;
  method AddFirst/AddLast(item: T): LinkedListNode<T>;
  method AddBefore/AddAfter(node: LinkedListNode<T>, item: T): LinkedListNode<T>;
  method RemoveFirst();  RemoveLast();  RemoveNode(node);  Find(item: T): LinkedListNode<T>?;
end
public class LinkedListNode<T>   public readonly property Value: T;  Next, Previous: LinkedListNode<T>?;  end
public class OwningLinkedList<T>: IOwningCollection<T> where T: class
  constructor();                     // NO source constructor — deliberate. Same shape with `owned item` on
  method PopFirst(): owned T;        //   AddFirst/AddLast/AddBefore/AddAfter; Remove*/Clear dispose.
  method PopLast(): owned T;   TryPopFirst/TryPopLast(out owned item: T?): bool;
  method Drain(): IOwningEnumerable<T>;
end
