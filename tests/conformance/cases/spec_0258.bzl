public async method ProcessFileAsync(path: string): Task
  await for var line in ReadLinesAsync(path) do
    Process(line);
  end;
end ProcessFileAsync;
