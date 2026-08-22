public class Outer
  private class NestedPrivate
    // ...
  end NestedPrivate;
  
  // ERROR: _x is internal, but NestedPrivate is private — the field's
  //        type is not as visible as the field.
  internal _x: NestedPrivate;

  // OK: also private, matches NestedPrivate's visibility.
  private _y: NestedPrivate;
end Outer;
