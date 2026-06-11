// Pattern 1: equalize to borrowed — both branches bind x to a source
// that outlives the merge.
var x: Stream;                       // x will be a borrow at the merge
if condition then
  using var scratch := new Stream(); // owned: used and discharged in-branch
  scratch.WarmUp();
  primary.Reset(scratch);            // scratch's work is done here
  x := primary.Stream;               // x borrows primary (outlives the if)
else
  x := otherSource.Stream;           // x borrows otherSource (outlives the if)
end;
// At the merge, x is borrowed on both branches, and both sources
// (primary, otherSource) outlive the merge point. The using var
// scratch was discharged at its own scope end; x never borrowed it.

// Pattern 2: produce an owned value on both branches.
var x: Stream;
if condition then
  x := new Stream();
else
  x := otherSource.CloneStream();   // returns owned Stream
end;
// At the merge, x is owned on both branches.
// x must reach an owned slot or be disposed before going out of scope.
