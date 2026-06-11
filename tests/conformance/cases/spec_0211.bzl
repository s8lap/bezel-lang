public class Box<T>
  public Value: T;
  // No destructor, no owned fields. Box<T> is trivial-dispose for any T.
end Box;

public class OwnedBox<T> where T: class
  public owned Value: T;
  // Has an owned field. OwnedBox<T> is NOT trivial-dispose for any T.
end OwnedBox;

_ := new Box<int>();              // OK (trivial-dispose)
_ := new Box<MyClass>();          // OK (trivial-dispose; Box doesn't own its T)
_ := new OwnedBox<MyClass>();     // ERROR (owned T field; non-trivial-dispose)
