/* ===========================================================================
 *  BoltJS built-ins -- included at the tail of kernel/js.c (shares its statics).
 *  Member access (.length, properties, DOM fields) and method dispatch for
 *  strings, arrays, and the host singletons console / Math / document / JSON,
 *  plus the global functions a page script reaches for.
 * ===========================================================================*/

static JVal wrap_dom(Interp *I, js_dom_node d) {
    if (!d) return jnull();
    JObj *o = new_obj(I, K_OBJ); if (!o) return jnull();
    o->dom = d; return jobj(o);
}
static char *join_args(Interp *I, JVal *a, int n, const char *sep) {
    char *s = ""; for (int i=0;i<n;i++){ if(i) s=str_concat(I,s,sep); s=str_concat(I,s,val_to_str(I,a[i])); } return s;
}
static char *substr_dup(Interp *I, const char *s, int start, int len) {
    if (start<0) start=0; int L=strlen(s); if(start>L)start=L;
    if (len<0) len=0; if (start+len>L) len=L-start;
    return arena_str(&I->a, s+start, len);
}
static int str_index_of(const char *hay, const char *needle, int from) {
    int hl=strlen(hay), nl=strlen(needle); if(nl==0) return from<=hl?from:hl;
    for (int i=from<0?0:from; i+nl<=hl; i++){ int j=0; for(;j<nl;j++) if(hay[i+j]!=needle[j])break; if(j==nl)return i; }
    return -1;
}

static int member_get(Interp *I, JVal recv, const char *name, JVal *out) {
    if (recv.t==T_STR) {
        if (strcmp(name,"length")==0){ *out=jnum(strlen(recv.u.str)); return 1; }
        return 0;                              /* methods via method_call */
    }
    if (recv.t==T_OBJ) {
        JObj *o=recv.u.obj;
        if (o->kind==K_ARR && strcmp(name,"length")==0){ *out=jnum(o->len); return 1; }
        if (o->dom && I->host) {
            if (strcmp(name,"innerHTML")==0||strcmp(name,"textContent")==0||strcmp(name,"innerText")==0) {
                char buf[8192]; buf[0]=0;
                I->host->get_inner(I->host->ud,o->dom,buf,sizeof buf);
                *out=jstr(arena_str(&I->a,buf,strlen(buf))); return 1;
            }
        }
        JVal v; if (obj_get(o,name,&v)){ *out=v; return 1; }
        if (o->htag==H_DOCUMENT) {
            if (strcmp(name,"body")==0 || strcmp(name,"documentElement")==0) {
                *out = I->host? wrap_dom(I,I->host->get_by_tag(I->host->ud,"body",0)) : jnull(); return 1;
            }
        }
        if (o->htag==H_WINDOW) {
            /* window.X resolves to a global of that name */
            Prop *p=env_find(I->global,name); if(p){ *out=p->val; return 1; }
        }
        return 0;
    }
    return 0;
}

static JVal method_call(Interp *I, JVal recv, const char *name, JVal *args, int nargs, int *handled) {
    *handled=1;
    JVal a0 = nargs>0?args[0]:jundef();
    JVal a1 = nargs>1?args[1]:jundef();

    if (recv.t==T_STR) {
        const char *s=recv.u.str; int L=strlen(s);
        if (strcmp(name,"charAt")==0){ int i=(int)to_num(a0); if(i<0||i>=L)return jstr(""); char c[2]={s[i],0}; return jstr(arena_str(&I->a,c,1)); }
        if (strcmp(name,"charCodeAt")==0){ int i=(int)to_num(a0); if(i<0||i>=L)return jnum(0); return jnum((unsigned char)s[i]); }
        if (strcmp(name,"indexOf")==0) return jnum(str_index_of(s,val_to_str(I,a0),nargs>1?(int)to_num(a1):0));
        if (strcmp(name,"includes")==0) return jbool(str_index_of(s,val_to_str(I,a0),0)>=0);
        if (strcmp(name,"startsWith")==0){ const char*n=val_to_str(I,a0); return jbool(strncmp(s,n,strlen(n))==0); }
        if (strcmp(name,"endsWith")==0){ const char*n=val_to_str(I,a0); int nl=strlen(n); return jbool(nl<=L && strcmp(s+L-nl,n)==0); }
        if (strcmp(name,"toUpperCase")==0){ char*r=arena_str(&I->a,s,L); for(int i=0;i<L;i++) if(r[i]>='a'&&r[i]<='z')r[i]-=32; return jstr(r); }
        if (strcmp(name,"toLowerCase")==0){ char*r=arena_str(&I->a,s,L); for(int i=0;i<L;i++) if(r[i]>='A'&&r[i]<='Z')r[i]+=32; return jstr(r); }
        if (strcmp(name,"trim")==0){ int b=0,e=L; while(b<e&&(s[b]==' '||s[b]=='\t'||s[b]=='\n'||s[b]=='\r'))b++; while(e>b&&(s[e-1]==' '||s[e-1]=='\t'||s[e-1]=='\n'||s[e-1]=='\r'))e--; return jstr(arena_str(&I->a,s+b,e-b)); }
        if (strcmp(name,"slice")==0||strcmp(name,"substring")==0){ int st=(int)to_num(a0); int en=nargs>1?(int)to_num(a1):L; if(st<0)st+=L; if(en<0)en+=L; if(st<0)st=0; if(en>L)en=L; if(en<st)en=st; return jstr(substr_dup(I,s,st,en-st)); }
        if (strcmp(name,"substr")==0){ int st=(int)to_num(a0); int ln=nargs>1?(int)to_num(a1):L-st; return jstr(substr_dup(I,s,st,ln)); }
        if (strcmp(name,"repeat")==0){ int k=(int)to_num(a0); char*r=""; for(int i=0;i<k;i++)r=str_concat(I,r,s); return jstr(r); }
        if (strcmp(name,"concat")==0){ char*r=arena_str(&I->a,s,L); for(int i=0;i<nargs;i++)r=str_concat(I,r,val_to_str(I,args[i])); return jstr(r); }
        if (strcmp(name,"replace")==0){ const char*from=val_to_str(I,a0),*to=val_to_str(I,a1); int idx=str_index_of(s,from,0); if(idx<0)return jstr(arena_str(&I->a,s,L)); char*r=substr_dup(I,s,0,idx); r=str_concat(I,r,to); r=str_concat(I,r,s+idx+strlen(from)); return jstr(r); }
        if (strcmp(name,"split")==0){ JObj*arr=new_obj(I,K_ARR); const char*sep=nargs>0?val_to_str(I,a0):0;
            if(!sep||sep[0]==0){ for(int i=0;i<L;i++){ char c[2]={s[i],0}; arr_push(I,arr,jstr(arena_str(&I->a,c,1))); } }
            else { int sl=strlen(sep),start=0; for(;;){ int idx=str_index_of(s,sep,start); if(idx<0){ arr_push(I,arr,jstr(arena_str(&I->a,s+start,L-start))); break; } arr_push(I,arr,jstr(substr_dup(I,s,start,idx-start))); start=idx+sl; } }
            return jobj(arr); }
        if (strcmp(name,"toString")==0) return recv;
        *handled=0; return jundef();
    }

    if (recv.t==T_NUM) {
        if (strcmp(name,"toString")==0) return jstr(i64_to_str(I,recv.u.num));
        if (strcmp(name,"toFixed")==0)  return jstr(i64_to_str(I,recv.u.num));
        *handled=0; return jundef();
    }

    if (recv.t==T_OBJ) {
        JObj *o=recv.u.obj;

        if (o->kind==K_ARR) {
            if (strcmp(name,"push")==0){ for(int i=0;i<nargs;i++)arr_push(I,o,args[i]); return jnum(o->len); }
            if (strcmp(name,"pop")==0){ if(o->len==0)return jundef(); return o->items[--o->len]; }
            if (strcmp(name,"shift")==0){ if(o->len==0)return jundef(); JVal v=o->items[0]; for(int i=1;i<o->len;i++)o->items[i-1]=o->items[i]; o->len--; return v; }
            if (strcmp(name,"unshift")==0){ for(int k=nargs-1;k>=0;k--){ arr_push(I,o,jundef()); for(int i=o->len-1;i>0;i--)o->items[i]=o->items[i-1]; o->items[0]=args[k]; } return jnum(o->len); }
            if (strcmp(name,"join")==0){ const char*sep=nargs>0?val_to_str(I,a0):","; char*r=""; for(int i=0;i<o->len;i++){ if(i)r=str_concat(I,r,sep); r=str_concat(I,r,val_to_str(I,o->items[i])); } return jstr(r); }
            if (strcmp(name,"indexOf")==0){ for(int i=0;i<o->len;i++) if(strict_eq(o->items[i],a0))return jnum(i); return jnum(-1); }
            if (strcmp(name,"includes")==0){ for(int i=0;i<o->len;i++) if(strict_eq(o->items[i],a0))return jbool(1); return jbool(0); }
            if (strcmp(name,"slice")==0){ int st=(int)to_num(a0); int en=nargs>1?(int)to_num(a1):o->len; if(st<0)st+=o->len; if(en<0)en+=o->len; if(st<0)st=0; if(en>o->len)en=o->len; JObj*r=new_obj(I,K_ARR); for(int i=st;i<en;i++)arr_push(I,r,o->items[i]); return jobj(r); }
            if (strcmp(name,"concat")==0){ JObj*r=new_obj(I,K_ARR); for(int i=0;i<o->len;i++)arr_push(I,r,o->items[i]); for(int k=0;k<nargs;k++){ if(args[k].t==T_OBJ&&args[k].u.obj->kind==K_ARR){ JObj*x=args[k].u.obj; for(int i=0;i<x->len;i++)arr_push(I,r,x->items[i]); } else arr_push(I,r,args[k]); } return jobj(r); }
            if (strcmp(name,"reverse")==0){ for(int i=0,j=o->len-1;i<j;i++,j--){ JVal t=o->items[i]; o->items[i]=o->items[j]; o->items[j]=t; } return recv; }
            if (strcmp(name,"forEach")==0 && a0.t==T_OBJ){ for(int i=0;i<o->len;i++){ JVal ar[2]={o->items[i],jnum(i)}; call_fn(I,a0.u.obj,jundef(),ar,2); if(I->errflag)break; } return jundef(); }
            if (strcmp(name,"map")==0 && a0.t==T_OBJ){ JObj*r=new_obj(I,K_ARR); for(int i=0;i<o->len;i++){ JVal ar[2]={o->items[i],jnum(i)}; arr_push(I,r,call_fn(I,a0.u.obj,jundef(),ar,2)); if(I->errflag)break; } return jobj(r); }
            if (strcmp(name,"filter")==0 && a0.t==T_OBJ){ JObj*r=new_obj(I,K_ARR); for(int i=0;i<o->len;i++){ JVal ar[2]={o->items[i],jnum(i)}; if(truthy(call_fn(I,a0.u.obj,jundef(),ar,2)))arr_push(I,r,o->items[i]); if(I->errflag)break; } return jobj(r); }
            if (strcmp(name,"find")==0 && a0.t==T_OBJ){ for(int i=0;i<o->len;i++){ JVal ar[2]={o->items[i],jnum(i)}; if(truthy(call_fn(I,a0.u.obj,jundef(),ar,2)))return o->items[i]; } return jundef(); }
            if (strcmp(name,"some")==0 && a0.t==T_OBJ){ for(int i=0;i<o->len;i++){ JVal ar[2]={o->items[i],jnum(i)}; if(truthy(call_fn(I,a0.u.obj,jundef(),ar,2)))return jbool(1); } return jbool(0); }
            if (strcmp(name,"every")==0 && a0.t==T_OBJ){ for(int i=0;i<o->len;i++){ JVal ar[2]={o->items[i],jnum(i)}; if(!truthy(call_fn(I,a0.u.obj,jundef(),ar,2)))return jbool(0); } return jbool(1); }
            if (strcmp(name,"reduce")==0 && a0.t==T_OBJ){ JVal acc; int start=0; if(nargs>1){acc=a1;}else{ if(o->len==0)return jundef(); acc=o->items[0]; start=1; } for(int i=start;i<o->len;i++){ JVal ar[3]={acc,o->items[i],jnum(i)}; acc=call_fn(I,a0.u.obj,jundef(),ar,3); if(I->errflag)break; } return acc; }
            if (strcmp(name,"toString")==0||strcmp(name,"join")==0) return jstr(val_to_str(I,recv));
            *handled=0; return jundef();
        }

        if (o->htag==H_CONSOLE) {
            if (strcmp(name,"log")==0||strcmp(name,"error")==0||strcmp(name,"warn")==0||strcmp(name,"info")==0||strcmp(name,"debug")==0) {
                if (I->host) I->host->log(I->host->ud, join_args(I,args,nargs," "));
                return jundef();
            }
            *handled=0; return jundef();
        }
        if (o->htag==H_MATH) {
            int64_t x=to_num(a0);
            if (strcmp(name,"floor")==0||strcmp(name,"ceil")==0||strcmp(name,"round")==0||strcmp(name,"trunc")==0) return jnum(x);
            if (strcmp(name,"abs")==0) return jnum(x<0?-x:x);
            if (strcmp(name,"sign")==0) return jnum(x>0?1:x<0?-1:0);
            if (strcmp(name,"sqrt")==0){ if(x<0)return jnum(0); int64_t r=0; while((r+1)*(r+1)<=x)r++; return jnum(r); }
            if (strcmp(name,"pow")==0){ int64_t b=x,e=to_num(a1),r=1; for(int64_t i=0;i<e&&i<64;i++)r*=b; return jnum(r); }
            if (strcmp(name,"max")==0){ int64_t m=nargs?to_num(args[0]):0; for(int i=1;i<nargs;i++){ int64_t v=to_num(args[i]); if(v>m)m=v; } return jnum(m); }
            if (strcmp(name,"min")==0){ int64_t m=nargs?to_num(args[0]):0; for(int i=1;i<nargs;i++){ int64_t v=to_num(args[i]); if(v<m)m=v; } return jnum(m); }
            if (strcmp(name,"random")==0) return jnum(0);     /* no float: best we can do */
            *handled=0; return jundef();
        }
        if (o->htag==H_DOCUMENT) {
            if (!I->host){ return jnull(); }
            if (strcmp(name,"getElementById")==0) return wrap_dom(I,I->host->get_by_id(I->host->ud,val_to_str(I,a0)));
            if (strcmp(name,"getElementsByTagName")==0||strcmp(name,"querySelectorAll")==0){ JObj*r=new_obj(I,K_ARR); for(int i=0;i<64;i++){ js_dom_node d=I->host->get_by_tag(I->host->ud,val_to_str(I,a0),i); if(!d)break; arr_push(I,r,wrap_dom(I,d)); } return jobj(r); }
            if (strcmp(name,"querySelector")==0){ const char*sel=val_to_str(I,a0); if(sel[0]=='#') return wrap_dom(I,I->host->get_by_id(I->host->ud,sel+1)); return wrap_dom(I,I->host->get_by_tag(I->host->ud,sel,0)); }
            if (strcmp(name,"write")==0||strcmp(name,"writeln")==0){ I->host->doc_write(I->host->ud,join_args(I,args,nargs,"")); if(strcmp(name,"writeln")==0)I->host->doc_write(I->host->ud,"\n"); return jundef(); }
            if (strcmp(name,"createElement")==0){ return wrap_dom(I,0); }
            if (strcmp(name,"addEventListener")==0) return jundef();
            *handled=0; return jundef();
        }
        if (o->htag==H_JSON) {
            if (strcmp(name,"stringify")==0) { /* minimal: numbers/strings/bools/arrays */
                return jstr(val_to_str(I,a0)); }
            if (strcmp(name,"parse")==0) { return jundef(); }
            *handled=0; return jundef();
        }
        if (o->dom && I->host) {
            if (strcmp(name,"setAttribute")==0||strcmp(name,"removeAttribute")==0||strcmp(name,"addEventListener")==0||strcmp(name,"appendChild")==0||strcmp(name,"focus")==0||strcmp(name,"click")==0) return jundef();
            if (strcmp(name,"getAttribute")==0) return jnull();
            *handled=0; return jundef();
        }

        /* user function stored as a property: let caller handle it */
        *handled=0; return jundef();
    }

    *handled=0; return jundef();
}

/* ----------------------------- global natives -------------------------- */
static JVal nf_parseInt(Interp *I, JVal t, JVal *a, int n){ (void)I;(void)t; if(n==0)return jnum(0); return jnum(to_num(a[0])); }
static JVal nf_parseFloat(Interp *I, JVal t, JVal *a, int n){ (void)I;(void)t; if(n==0)return jnum(0); return jnum(to_num(a[0])); }
static JVal nf_isNaN(Interp *I, JVal t, JVal *a, int n){ (void)I;(void)t; (void)a;(void)n; return jbool(0); }
static JVal nf_String(Interp *I, JVal t, JVal *a, int n){ (void)t; if(n==0)return jstr(""); return jstr(val_to_str(I,a[0])); }
static JVal nf_Number(Interp *I, JVal t, JVal *a, int n){ (void)I;(void)t; if(n==0)return jnum(0); return jnum(to_num(a[0])); }
static JVal nf_Boolean(Interp *I, JVal t, JVal *a, int n){ (void)I;(void)t; if(n==0)return jbool(0); return jbool(truthy(a[0])); }
static JVal nf_Array(Interp *I, JVal t, JVal *a, int n){ (void)t; JObj*r=new_obj(I,K_ARR); if(n==1&&a[0].t==T_NUM){ int k=(int)a[0].u.num; for(int i=0;i<k;i++)arr_push(I,r,jundef()); } else for(int i=0;i<n;i++)arr_push(I,r,a[i]); return jobj(r); }
static JVal nf_alert(Interp *I, JVal t, JVal *a, int n){ (void)t; if(I->host)I->host->log(I->host->ud,join_args(I,a,n," ")); return jundef(); }
static JVal nf_noop(Interp *I, JVal t, JVal *a, int n){ (void)I;(void)t;(void)a;(void)n; return jundef(); }

static void def_native(Interp *I, const char *name, NativeFn fn) {
    JObj *o=new_obj(I,K_NATIVE); if(!o)return; o->nat=fn; env_def(I,I->global,name,jobj(o));
}
static JObj *host_obj(Interp *I, int htag) { JObj *o=new_obj(I,K_OBJ); if(o)o->htag=htag; return o; }

static void js_install_globals(Interp *I) {
    env_def(I,I->global,"console", jobj(host_obj(I,H_CONSOLE)));
    env_def(I,I->global,"Math",    jobj(host_obj(I,H_MATH)));
    env_def(I,I->global,"document",jobj(host_obj(I,H_DOCUMENT)));
    env_def(I,I->global,"JSON",    jobj(host_obj(I,H_JSON)));
    JObj *win=host_obj(I,H_WINDOW);
    env_def(I,I->global,"window",  jobj(win));
    env_def(I,I->global,"globalThis", jobj(win));
    env_def(I,I->global,"undefined", jundef());
    env_def(I,I->global,"NaN", jnum(0));
    env_def(I,I->global,"Infinity", jnum(0x7fffffffffffffffLL));
    def_native(I,"parseInt",nf_parseInt);
    def_native(I,"parseFloat",nf_parseFloat);
    def_native(I,"isNaN",nf_isNaN);
    def_native(I,"String",nf_String);
    def_native(I,"Number",nf_Number);
    def_native(I,"Boolean",nf_Boolean);
    def_native(I,"Array",nf_Array);
    def_native(I,"alert",nf_alert);
    def_native(I,"setTimeout",nf_noop);
    def_native(I,"setInterval",nf_noop);
    def_native(I,"addEventListener",nf_noop);
}
