method Import(path: string)
  using var fs := new FileStream(path, FileMode.Open);
  using var parser := new Parser(fs);
  parser.ReadAll();                // if this throws, parser then fs are
end Import;                        // disposed automatically, innermost first
