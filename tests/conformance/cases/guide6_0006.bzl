// private owned _items: Array<string>;
public method Discard(count: int)
  requires(count >= 0)
  requires(count <= _items.Count)
  ensures(_items.Count = old(_items.Count) - count)

  for var i in 0..<count do
    _items.RemoveAt(_items.Count - 1);
  end;
end Discard;
