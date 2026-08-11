class Handle
  private _c: Cell;            // borrow field (no `owned`)
  public constructor()
    _c := new Cell();          // ERROR: a fresh owned value stored into a
  end constructor;             //   non-`owned` field has no owner — declare `owned _c`.

  public property Item: Cell
    get => new Cell();         // ERROR: a getter may not return a fresh owned
  end Item;                    //   value — use `method MakeItem(): owned Cell`.
end Handle;
