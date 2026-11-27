import os
HERE=os.path.dirname(os.path.abspath(__file__))
import re,sys,glob,os
src=HERE+'/../../source/'
files=[f for f in sorted(glob.glob(src+'*.c')+glob.glob(src+'*.inc')+glob.glob(src+'*.h')) if os.path.basename(f) not in('font_data.c','qrcodegen.c','qrcodegen.h')]
lit=re.compile(r'"((?:[^"\\\n]|\\.)*)"(?:\s*"((?:[^"\\\n]|\\.)*)")*')
def dec(s):
    out=bytearray();i=0
    while i<len(s):
        c=s[i]
        if c=='\\':
            n=s[i+1]
            if n=='x':
                j=i+2;h=''
                while j<len(s) and len(h)<2 and s[j] in '0123456789abcdefABCDEF':h+=s[j];j+=1
                out.append(int(h,16));i=j;continue
            if n in '01234567':
                j=i+1;o=''
                while j<len(s) and len(o)<3 and s[j] in '01234567':o+=s[j];j+=1
                out.append(int(o,8));i=j;continue
            out+={'n':b'\n','t':b'\t','"':b'"','\\':b'\\',"'":b"'",'r':b'\r'}.get(n,n.encode());i+=2;continue
        out+=c.encode();i+=1
    return out.decode('utf8',errors='replace')
if __name__=='__main__':
    for f in files:
        for ln,l in enumerate(open(f,encoding='utf8',errors='replace'),1):
            for m in re.finditer(r'(?:"(?:[^"\\\n]|\\.)*"\s*)+',l):
                parts=re.findall(r'"((?:[^"\\\n]|\\.)*)"',m.group(0))
                s=dec(''.join(parts))
                if re.search(r'[A-Za-z\u00c0-\u00ff]{3}',s): print(f"{os.path.basename(f)}:{ln}: {s!r}")
