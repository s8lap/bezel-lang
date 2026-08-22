public class Set<T>: ICollection<T>                  // T must be hashable
  constructor();  (capacity);  (comparer);  (capacity, comparer);
  method TryAdd(item: T): bool;
  method UnionWith / IntersectWith / ExceptWith(other: IEnumerable<T>);
  method IsSubsetOf / IsSupersetOf(other: IEnumerable<T>): bool;
end
public class OwningSet<T>: IOwningCollection<T> where T: class
  constructor();  (capacity);  (comparer);  (capacity, comparer);
  method TryAdd(owned item: T): bool;          // on false (duplicate) the passed item is DISPOSED
  method Pop(query: T): owned T;   TryPop(query: T, out owned removed: T?): bool;
  method Drain(): IOwningEnumerable<T>;
  method UnionWith(owned other: OwningSet<T>);       // DRAINS other; duplicates disposed
  method IntersectWith(other: IEnumerable<T>);       // disposes elements not in other
  method ExceptWith(other: IEnumerable<T>);          // disposes elements in other
  method IsSubsetOf / IsSupersetOf(other: IEnumerable<T>): bool;
end

// Stack / Queue — all four conform to IReadOnlyCollection<T> ONLY: no Add, no Remove, no IsReadOnly
Stack<T>:        Push(item: T);  Pop(): T;  pure Peek(): T;  TryPop(out item: T?): bool;
                 TryPeek(out item: T?): bool;  Contains(item: T): bool;  Clear();
OwningStack<T>:  Push(owned item: T);  Pop(): owned T;  pure Peek(): T;  TryPop(out owned item: T?): bool;
                 TryPeek(out item: T?): bool;  Contains;  Clear();  Drain(): IOwningEnumerable<T>;
Queue<T>:        Enqueue(item: T);  Dequeue(): T;  pure Peek(): T;  TryDequeue(out item: T?): bool;
                 TryPeek(out item: T?): bool;  Contains;  Clear();
OwningQueue<T>:  Enqueue(owned item: T);  Dequeue(): owned T;  pure Peek(): T;
                 TryDequeue(out owned item: T?): bool;  TryPeek(out item: T?): bool;  Contains;  Clear();
                 Drain(): IOwningEnumerable<T>;
