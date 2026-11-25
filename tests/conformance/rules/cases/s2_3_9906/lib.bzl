module Lib;

public class FrozenArray<T>
  private _items: Array<T>;

  public constructor(items: Array<T>)
    this._items := items;
  end constructor;

  public property [i: int]: T
    get => this._items[i];
    set => do this._items[i] := value; end;
  end;
end FrozenArray;
