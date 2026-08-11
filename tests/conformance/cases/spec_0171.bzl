extension string
  public property IsBlank: bool => this.Length = 0 or this.Trim().Length = 0;
  public method Words(): owned Array<string> => this.Split(' ');
end;
