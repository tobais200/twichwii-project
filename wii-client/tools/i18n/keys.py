import os
HERE=os.path.dirname(os.path.abspath(__file__))
import re,sys,os,json
sys.path.insert(0,HERE+'')
from extract import files,dec
def keys():
    ks={}
    for f in files:
        fn=os.path.basename(f)
        if fn.startswith('i18n'): continue
        txt=open(f,encoding='utf8',errors='surrogateescape').read()
        for m in re.finditer(r'\bT\(((?:\s*"(?:[^"\\\n]|\\.)*")+)\s*\)',txt):
            parts=re.findall(r'"((?:[^"\\\n]|\\.)*)"',m.group(1))
            ks.setdefault(dec(''.join(parts)),fn)
        if fn=='main.c':
            for blk in ('help_parts','about_parts'):
                a=txt.index('static const char *const %s[]'%blk); b=txt.index('NULL };',a)
                for mm in re.finditer(r'((?:\s*"(?:[^"\\\n]|\\.)*")+)\s*,',txt[a:b]):
                    parts=re.findall(r'"((?:[^"\\\n]|\\.)*)"',mm.group(1)); ks.setdefault(dec(''.join(parts)),'main.c/'+blk)
            for m in re.finditer(r'const char \*const lp\[\] = \{(.*?)NULL',txt,re.S):
                for x in re.findall(r'"((?:[^"\\\n]|\\.)*)"',m.group(1)): ks.setdefault(dec(x),'main.c/legend')
            for m in re.finditer(r'static const char \*const c\[\] = \{(.*?)\};',txt,re.S):
                for x in re.findall(r'"((?:[^"\\\n]|\\.)*)"',m.group(1)): ks.setdefault(dec(x),'main.c/display')
        if fn=='twitchapi.c':
            for x in ['Proxy 360p','Transcodificado 270p','Directo (HTTPS)']: ks.setdefault(x,'twitchapi.c/pm')
    for x in ['Sin sesion','Estado sin consultar']: ks.setdefault(x,'init')
    ks.pop('',None)
    return ks
if __name__=='__main__':
    ks=keys()
    json.dump(ks,open(HERE+'/keys.json','w'),ensure_ascii=False,indent=0)
    print(len(ks))
    for k,v in ks.items(): print(v,repr(k))
