public class Box<T>(value: T)
  public Value: T := value;
  // No destructor, no owned fields. Box<T> is trivial-dispose for any T.
end Box;

public class OwningBox<T>(owned value: T) where T: class
  public owned Value: T := value;
  // Has an owned field of T. OwningBox<T> is trivial-dispose exactly when the
  // substituted T is (§3.6 rule 2): the synthesized cascade over an owned
  // trivial-dispose field runs no user code.
end OwningBox;

using var m := new MyClass();
_ := new Box<int>(0);                          // OK (trivial-dispose)
_ := new Box<MyClass>(m);                      // OK (trivial-dispose; Box doesn't own its T — it borrows m)
_ := new OwningBox<MyClass>(new MyClass());    // OK (disposes the owned T field at end of statement, §17.13)
