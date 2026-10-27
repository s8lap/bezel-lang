import json,re,sys
old,new,d=sys.argv[1],sys.argv[2],sys.argv[3]   # e.g. 0.325 0.326 scratch/v0326
nums=json.load(open(d+'/nums.json'))
e=open(d+'/entry.md').read()
e=re.sub(r'\{\{ISSUE:([^}]*)\}\}', lambda m: '[#%d](https://github.com/s8lap/bezel-lang/issues/%d)'%(nums[m.group(1)],nums[m.group(1)]), e)
assert '{{' not in e
def sub1(f,a,b):
    s=open(f).read(); assert s.count(a)==1,(f,a,s.count(a)); open(f,'w').write(s.replace(a,b))
sub1("docs/Bezel_Language_Spec.md",f"**Version {old} — October 2026**",f"**Version {new} — October 2026**")
for f in ["docs/Bezel_Standard_Library.md","docs/Bezel_Design_Rationale.md","docs/Bezel_Changelog.md"]:
    sub1(f,f"**Companion to the Bezel Language Specification, v{old} — October 2026**",f"**Companion to the Bezel Language Specification, v{new} — October 2026**")
cg="docs/Bezel_For_Code_Generation.md"
for a in [f"*Validated against v{old}.",f"are to spec v{old} and `Bezel_Standard_Library.md` v{old}",f"transcribed from `Bezel_Standard_Library.md` v{old}"]:
    sub1(cg,a,a.replace(old,new))
e=e.rstrip()
if e.endswith('---'): e=e[:-3].rstrip()
c=open("docs/Bezel_Changelog.md").read().rstrip()+"\n\n"+e+"\n\n---\n"
open("docs/Bezel_Changelog.md","w").write(c)
print("finalized",new)
