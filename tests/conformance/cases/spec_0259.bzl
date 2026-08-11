await for var line in ReadLinesAsync(path) with token do
  Process(line);
end;
