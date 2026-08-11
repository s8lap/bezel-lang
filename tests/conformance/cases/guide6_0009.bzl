public static pure method CountOf(text: string, needle: char): int
  var n := 0;                       // frame-local mutation: root is a local
  for var c in text do              // concrete string source — direct-indexed,
    if c = needle then              //   admitted; no enumerator is built
      n := n + 1;
    end;
  end;
  return n;
end CountOf;
