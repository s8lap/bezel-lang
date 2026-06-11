method Import(path: string)
  using var file := new FileStream(path, FileMode.Open);  // disposed second
  using var parser := new Parser(file);    // disposed first, even if ReadAll throws
  parser.ReadAll();
end Import;
