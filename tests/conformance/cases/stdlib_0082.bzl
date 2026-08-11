/// Compiler-intrinsic; T restricted to sendable value types and `string`.
public class FrozenArray<T>: IEnumerable<T>
  // Construction consumes or copies its source — after either, no writer can exist.
  public static method From(owned buffer: OwningBuffer<T>): owned FrozenArray<T>;
      // VALUE-TYPED T ONLY: O(1) — steals the storage; the buffer is consumed
      //   (the call-site transfer marker `move`; the proposal-0006 owned-vs-copy construction shape).
      //   No string-element form can exist: OwningBuffer<T> does not admit string (§4.14).
  public static method From(items: Array<T>): owned FrozenArray<T>;
      // Copies; the only construction form for T = string.

  public readonly property Length: int;

  public property [i: int]: T
    get;                       // bounds-checked; IndexOutOfRangeException outside 0..<Length
  end;

  public method ToArray(): owned Array<T>;   // fresh mutable copy back out

  destructor;                  // releases the backing storage
end FrozenArray;
