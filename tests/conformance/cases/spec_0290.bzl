// §14.1 / fix #992: `new T()` inside a generic body without `where T: new()`
// must be a compile error.  `where T: class` is not sufficient.
module M;
class Widget
  public static total_ctors: int := 0;
  public constructor(v: int) Widget.total_ctors := Widget.total_ctors + 1; end constructor;
  destructor end destructor;
end Widget;
class Box<T> where T: class
  public method MakeOne(): T
    return new T();   // ERROR: T is not constrained where T: new() (§14.1)
  end MakeOne;
end Box;
class C public static method Main(): int
  using var b := new Box<Widget>();
  var w := b.MakeOne();
  return Widget.total_ctors;
end Main; end C;
