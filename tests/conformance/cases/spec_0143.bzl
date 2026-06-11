public class OwnedArray<T> where T: class
  public property [i: int]: T
    get;                     // implicit value: T, borrow-returning
    set(owned);              // explicit owned: takes owned T;
                             //   disposes previous slot value
  end;
end OwnedArray;
