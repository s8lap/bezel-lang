public property [i: int]: string => _items[i];        // expression-bodied

public property [row: int, col: int]: double => _grid[row * _width + col];

public property [key: string]: int
  get => _values[key];
  set => do
    if value < 0 then throw new ArgumentException("must be non-negative"); end;
    _values[key] := value;
  end;
end;
