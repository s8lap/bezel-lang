extension string
  public method IsBlank(): bool => this.Length = 0 or this.Trim().Length = 0;
  public property Words: Array<string> => this.Split(' ');
end;
