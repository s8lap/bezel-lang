for var entry in _index do
  entry.Value.Dispose();
end;
_index.Clear();
