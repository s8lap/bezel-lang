scan: for var row in rows do
  for var cell in row do
    if cell.Bad then break scan; end;
  end;
end scan;
