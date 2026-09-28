from __future__ import annotations
import sys, html, webbrowser, json, threading, re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from dataclasses import dataclass, field
from enum import Enum, auto
from pathlib import Path

SOURCE_TEXT:dict[str,str]={}
def source_lines(file:str)->list[str]:
    if file in SOURCE_TEXT: return SOURCE_TEXT[file].splitlines()
    try:
        text=Path(file).read_text(encoding='utf-8'); SOURCE_TEXT[file]=text; return text.splitlines()
    except OSError: return []

class C:
    RESET='\033[0m'; BOLD='\033[1m'; RED='\033[31m'; GREEN='\033[32m'; YELLOW='\033[33m'; PURPLE='\033[35m'; GRAY='\033[90m'; CYAN='\033[36m'; RED_UL='\033[4;31m'

def color(s: str, c: str) -> str: return f'{c}{s}{C.RESET}'
def kw(s: str) -> str: return color(s,C.PURPLE)
def green(s: str) -> str: return color(s,C.GREEN)
def yellow(s: str) -> str: return color(s,C.YELLOW)
def gray(s: str) -> str: return color(s,C.GRAY)
def cyan(s: str) -> str: return color(s,C.CYAN)
def literal_text(s: str) -> str: return s.rsplit('::',1)[-1]
def is_string_literal(s: str) -> bool: return literal_text(s).startswith('\"') and literal_text(s).endswith('\"')
def is_numeric_literal(s: str) -> bool: return re.fullmatch(r'(?:[0-9]+(?:\.[0-9]+)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?', literal_text(s)) is not None
def is_literal(s: str) -> bool: return is_string_literal(s) or is_numeric_literal(s)
def typefmt(s: str) -> str:
    if '::' not in s: return yellow(s) if is_literal(s) else green(s)
    u,n=s.rsplit('::',1)
    return gray(u+'::')+(yellow(n) if is_literal(n) else green(n))

@dataclass(frozen=True)
class Pos:
    offset:int; line:int; column:int

@dataclass(frozen=True)
class Span:
    start:Pos; end:Pos

class Error(Exception):
    def __init__(self,file:str,span:Span,message:str,graph=None): self.file,self.span,self.message,self.graph=file,span,message,graph
    def pretty(self)->str:
        lines=source_lines(self.file)
        p=self.span.start; out=[f'{C.BOLD}{C.RED}error:{C.RESET} {C.RED}{self.message}{C.RESET}',f' {C.PURPLE}-->{C.RESET} {self.file}:{p.line}:{p.column}']
        if 1<=p.line<=len(lines):
            s=lines[p.line-1]; a=max(0,p.column-1); n=max(1,self.span.end.column-p.column) if self.span.end.line==p.line else 1; b=min(len(s),a+n); marked=s[:a]+C.RED_UL+s[a:b]+C.RESET+s[b:]; w=len(str(p.line)); out += [f" {' '*w} {C.PURPLE}|{C.RESET}",f' {p.line} {C.PURPLE}|{C.RESET} {marked}',f" {' '*w} {C.PURPLE}|{C.RESET} {' '*a}{C.RED}{'^'*max(1,b-a)}{C.RESET}"]
        if self.graph is not None:
            try: out += [f' {C.PURPLE}|{C.RESET}', color(' graph context:',C.BOLD), graph_diagnostic(self.graph)]
            except Exception: pass
        return '\n'.join(out)

class Kind(Enum):
    NAME=auto(); STRING=auto(); REL=auto(); COLON=auto(); PIPE=auto(); UNIVERSE=auto(); USES=auto(); DEF=auto(); RETURN=auto(); LPAR=auto(); RPAR=auto(); WHERE=auto(); REDUCE=auto(); DO=auto(); RUN=auto(); IMPORT=auto(); ALL=auto(); EOL=auto(); COMMA=auto(); EOF=auto()

KEYWORDS={x:getattr(Kind,x.upper()) for x in ('universe','uses','def','return','where','reduce','do','run','import','all')}

@dataclass(frozen=True)
class Token:
    kind:Kind; text:str; span:Span

class Lexer:
    REL=set('~=<>!+-*/%^&@#$?\\()')
    def __init__(self,file:str,source:str): self.file,self.source,self.i,self.line,self.col=file,source,0,1,1; SOURCE_TEXT[file]=source
    def pos(self)->Pos: return Pos(self.i,self.line,self.col)
    def eof(self)->bool: return self.i>=len(self.source)
    def peek(self,n:int=0)->str: return self.source[self.i+n] if self.i+n<len(self.source) else ''
    def advance(self)->str:
        c=self.source[self.i]; self.i+=1
        if c=='\n': self.line,self.col=self.line+1,1
        else: self.col+=1
        return c
    def make(self,k:Kind,text:str,start:Pos)->Token: return Token(k,text,Span(start,self.pos()))
    def skip(self)->None:
        while True:
            while not self.eof() and self.peek() in ' \t\r': self.advance()
            if self.peek()!='/' or self.peek(1)!='/': return
            while not self.eof() and self.peek()!='\n': self.advance()
    def string(self)->Token:
        start,text=self.pos(),self.advance()
        while not self.eof():
            c=self.advance(); text+=c
            if c=='\\':
                if self.eof(): break
                text+=self.advance()
            elif c=='"': return self.make(Kind.STRING,text,start)
        raise Error(self.file,Span(start,self.pos()),'unterminated string literal')
    def name(self)->Token:
        start,text=self.pos(),''
        while not self.eof():
            c=self.peek()
            if c.isspace() or c in '|",' or c in self.REL: break
            if c==':':
                if self.peek(1)==':': text+=self.advance()+self.advance(); continue
                break
            text+=self.advance()
        if not text: raise Error(self.file,Span(start,self.pos()),f'unexpected character {self.peek()!r}')
        return self.make(KEYWORDS.get(text,Kind.NAME),text,start)
    def relation(self)->Token:
        start,text=self.pos(),''
        while not self.eof() and not self.peek().isspace() and self.peek() not in ':|"' and self.peek() in self.REL:
            if self.peek()=='/' and self.peek(1)=='/': break
            text+=self.advance()
        return self.make(Kind.REL,text,start)
    def next(self)->Token:
        self.skip(); start=self.pos()
        if self.eof(): return Token(Kind.EOF,'',Span(start,start))
        if self.peek()=='\n': self.advance(); return self.make(Kind.EOL,'\n',start)
        if self.peek()==',': self.advance(); return self.make(Kind.COMMA,',',start)
        if self.peek()=='"': return self.string()
        if self.peek()==':': self.advance(); return self.make(Kind.COLON,':',start)
        if self.peek()=='|': self.advance(); return self.make(Kind.PIPE,'|',start)
        if self.peek()=='(': self.advance(); return self.make(Kind.LPAR,'(',start)
        if self.peek()==')': self.advance(); return self.make(Kind.RPAR,'(',start)
        if self.peek() in self.REL: return self.relation()
        return self.name()
    def tokens(self)->list[Token]:
        out=[]
        while True:
            t=self.next(); out.append(t)
            if t.kind==Kind.EOF: return out

@dataclass
class Field:
    name:str; types:list[str]; span:Span

@dataclass
class Relation:
    left:str; tag:str; right:str; span:Span

@dataclass
class Reduce:
    ret:str; function:str; args:list[str]; span:Span; all:bool=False; relations:list[Relation]=field(default_factory=list)

Statement=Field|Relation

@dataclass
class Type:
    universe:str; name:str; inputs:list[Statement]=field(default_factory=list); outputs:list[Statement]|None=None; where:list[Relation|Reduce]|None=None; span:Span|None=None; return_all:bool=False; uses:list[str]=field(default_factory=list)
    @property
    def full(self)->str: return f'{self.universe}::{self.name}'
    @property
    def function(self)->bool: return self.outputs is not None

@dataclass
class Universe:
    name:str; types:list[Type]=field(default_factory=list)

@dataclass
class RunType:
    universe:str; name:str; span:Span; uses:list[str]=field(default_factory=list)

@dataclass
class Program:
    universes:list[Universe]=field(default_factory=list); runs:list[RunType]=field(default_factory=list); imports:list[tuple[str,Span]]=field(default_factory=list)

class Parser:
    def __init__(self,file:str,tokens:list[Token],importer=None):
        self.file,self.tokens,self.i,self.program,self.universe,self.type,self.mode,self.importer,self.temp,self.segment_group=file,tokens,0,Program([Universe('Global')]),None,None,'input',importer,0,None
        self.universe=self.program.universes[0]; self.uses=[]
    def peek(self,n:int=0)->Token: return self.tokens[min(self.i+n,len(self.tokens)-1)]
    def take(self)->Token:
        t=self.peek()
        if t.kind!=Kind.EOF: self.i+=1
        return t
    def expect(self,*kinds:Kind)->Token:
        t=self.peek()
        if t.kind not in kinds:
            expected=' or '.join(x.name.lower().replace('rel', "relation") for x in kinds)
            got=t.text if t.text else '<end of file>'
            raise Error(self.file,t.span,f"syntax error: expected {expected}, but found {got!r} ({t.kind.name.lower()}); check the surrounding variable, relation, type annotation, or delimiter")
        return self.take()
    def separators(self)->None:
        while self.peek().kind in (Kind.EOL,Kind.COMMA): self.take()
    def inner_eols(self)->None:
        while self.peek().kind==Kind.EOL: self.take()
    def finish(self)->None:
        if self.type is None: return
        if self.type.outputs is not None:
            names={x.name for x in self.type.inputs if isinstance(x,Field)}
            #for x in self.type.outputs:
            #    if isinstance(x,Field) and x.name in names: raise Error(self.file,x.span,f'{x.name!r} is both input and output')
        self.universe.types.append(self.type); self.type,self.mode=None,'input'
    def partial_program(self)->Program:
        if self.type is not None and self.universe is not None and self.type not in self.universe.types: self.universe.types.append(self.type)
        return self.program
    def open_universe(self)->None:
        self.finish(); self.take(); n=self.expect(Kind.NAME); self.universe=next((x for x in self.program.universes if x.name==n.text),None)
        if self.universe is None: self.universe=Universe(n.text); self.program.universes.append(self.universe)
        self.uses=[]
    def open_uses(self)->None:
        self.finish(); token=self.take(); n=self.expect(Kind.NAME)
        if n.text not in self.uses: self.uses.append(n.text)
    def group_segment(self,mode:str)->None:
        if self.peek().kind==Kind.LPAR: self.take(); self.segment_group=mode; self.inner_eols()
    def open_type(self)->None:
        self.finish(); token=self.take()
        if self.universe is None: raise Error(self.file,token.span,'def outside universe')
        n=self.expect(Kind.NAME,Kind.STRING)
        self.type,self.mode=Type(self.universe.name,n.text,span=Span(token.span.start,n.span.end),uses=list(self.uses)),'input'
        if self.peek().kind==Kind.COLON:
            self.take(); base=self.expect(Kind.NAME,Kind.STRING); self.type.inputs.append(Field('$',[base.text],Span(n.span.start,base.span.end)))
        self.group_segment('input')
    def open_return(self)->None:
        token=self.take()
        if self.type is None: raise Error(self.file,token.span,"'return' is not inside a type/function declaration; add it after a 'def ...' declaration")
        if self.type.outputs is not None: raise Error(self.file,token.span,f"duplicate return clause in {self.type.full!r}; a declaration may have only one 'return' or 'return all' clause")
        if self.type.where is not None: raise Error(self.file,token.span,f"return clause appears after 'where' in {self.type.full!r}; put 'return'/'return all' before the where block so the return policy is known while the graph is built")
        self.type.outputs,self.mode=[],'output'
        if self.peek().kind==Kind.ALL:
            self.take(); self.type.return_all=True
        self.group_segment('output')
    def open_where(self)->None:
        token=self.take()
        if self.type is None: raise Error(self.file,token.span,"runaway 'where'")
        if self.type.where is not None: raise Error(self.file,token.span,"duplicate 'where'")
        self.type.where,self.mode=[],'where'; self.group_segment('where')
    def temporary(self)->str:
        n=f'__tmp{self.temp}'; self.temp+=1; return n
    def declared_where_targets(self)->dict[str,Field]:
        """Names that a normal-return where block is allowed to assign into."""
        out={}
        if self.type is None: return out
        for ss in (self.type.inputs, self.type.outputs or []):
            for x in ss:
                if isinstance(x,Field): out.setdefault(x.name,x)
        return out
    def require_where_target(self,ret:str,span:Span,function:str)->None:
        if self.type is None or self.type.return_all: return
        root=ret.split('.',1)[0]
        declared=self.declared_where_targets()
        if root in declared: return
        outputs=sorted(x.name for x in (self.type.outputs or []) if isinstance(x,Field))
        inputs=sorted(x.name for x in self.type.inputs if isinstance(x,Field))
        if ret.startswith('__tmp'):
            detail=(f"temporary result {ret!r} created while evaluating {function!r} has no declared destination")
        else:
            detail=(f"assignment target {ret!r} for call {function!r} does not exist in the declared input/return graph")
        raise Error(self.file,span,
            f"{detail} in {self.type.full!r}. This declaration uses an explicit return policy, so a where block may only assign to variables already declared by the input or return shape. "
            f"Declared inputs: {', '.join(inputs) or '<none>'}. Declared return variables: {', '.join(outputs) or '<none>'}. "
            f"Declare {root!r} in the return shape, or use 'return all' if the where block is intended to introduce new named or temporary variables.")
    def call(self,ret:str,start:Pos,fn:Token|None=None)->None:
        fn=self.expect(Kind.NAME) if fn is None else fn
        if self.peek().kind!=Kind.LPAR:
            raise Error(self.file,self.peek().span,f"function call {fn.text!r} requires parentheses")
        self.take(); self.inner_eols(); args=[]; relations=[]
        while self.peek().kind!=Kind.RPAR:
            if self.peek().kind==Kind.EOF:
                raise Error(self.file,self.peek().span,f"expected ')' to close call to {fn.text!r}")
            item_start=self.peek().span.start
            # Nested calls are ordinary node arguments whose result is named by a temporary.
            if self.peek().kind==Kind.NAME and self.peek(1).kind==Kind.LPAR:
                nested_fn=self.take(); tmp=self.temporary(); self.call(tmp,item_start,nested_fn); args.append(tmp)
            else:
                left=self.expect(Kind.NAME,Kind.STRING)
                # A relation argument is written exactly like a relation statement, e.g. x before y.
                # Commas make this unambiguous from a plain node argument.
                if self.peek().kind in (Kind.REL,Kind.NAME):
                    op=self.take()
                    if op.text=='!' and self.peek().kind==Kind.NAME:
                        op=Token(Kind.NAME,'!'+self.take().text,Span(op.span.start,self.tokens[self.i-1].span.end))
                    right=self.expect(Kind.NAME,Kind.STRING)
                    relations.append(Relation(left.text,op.text,right.text,Span(left.span.start,right.span.end)))
                else:
                    args.append(left.text)
                    # Permit an optional call-site type annotation (x:Type). It is syntactic
                    # information only; graph/type matching remains authoritative.
                    if self.peek().kind==Kind.COLON:
                        self.take(); self.expect(Kind.NAME,Kind.STRING)
            self.inner_eols()
            if self.peek().kind==Kind.COMMA:
                self.take(); self.inner_eols()
                if self.peek().kind==Kind.RPAR: break
                continue
            if self.peek().kind!=Kind.RPAR:
                raise Error(self.file,self.peek().span,"expected ',' or ')' in function call")
        rp=self.expect(Kind.RPAR)
        if not args: raise Error(self.file,fn.span,f"function call {fn.text!r} requires at least one node argument inside parentheses; relation arguments describe edges but do not supply a value node")
        call_span=Span(start,rp.span.end)
        self.require_where_target(ret,call_span,fn.text)
        self.type.where.append(Reduce(ret,fn.text,args,call_span,False,relations))
    def reduce_statement(self)->None:
        token=self.take()
        if self.peek().kind==Kind.ALL:
            self.take(); fn=self.expect(Kind.NAME); self.type.where.append(Reduce(self.temporary(),fn.text,[],Span(token.span.start,fn.span.end),True)); return
        fn=self.expect(Kind.NAME); self.call(self.temporary(),token.span.start,fn)
    def statement(self)->None:
        if self.type is None: raise Error(self.file,self.peek().span,'statement is outside any declaration; start a declaration with def before adding variables or relations')
        if self.mode=='output' and self.type.return_all:
            raise Error(self.file,self.peek().span,
                f"{self.type.full!r} uses 'return all', so explicit return variables/relations are not allowed. "
                "Put derived variables in the subsequent where block; they will be collected automatically after that block is built")
        if self.mode=='where' and self.peek().kind==Kind.REDUCE: self.reduce_statement(); return
        if self.peek().kind==Kind.LPAR:
            self.take(); self.inner_eols(); ret=self.statement(); self.inner_eols(); self.expect(Kind.RPAR); return ret
        left=self.expect(Kind.NAME)
        if self.peek().kind==Kind.COLON:
            if self.mode=='where': raise Error(self.file,left.span,"field inside 'where'")
            self.take(); first=self.expect(Kind.NAME,Kind.STRING); types=[first.text]
            while self.peek().kind==Kind.PIPE: self.take(); types.append(self.expect(Kind.NAME,Kind.STRING).text)
            target=self.type.inputs if self.mode=='input' else self.type.outputs
            if any(isinstance(x,Field) and x.name==left.text for x in target): raise Error(self.file,left.span,f'duplicate field {left.text!r}')
            target.append(Field(left.text,types,Span(left.span.start,self.tokens[self.i-1].span.end))); return
        op=self.expect(Kind.REL,Kind.NAME)
        if op.text=='!' and self.peek().kind==Kind.NAME: op=Token(Kind.NAME,'!'+self.take().text,Span(op.span.start,self.tokens[self.i-1].span.end))
        if self.mode=='where' and op.text=='=':
            if self.peek().kind==Kind.REDUCE: self.take(); self.call(left.text,left.span.start); return
            if self.peek().kind==Kind.NAME and self.peek(1).kind==Kind.LPAR:
                fn=self.take(); self.call(left.text,left.span.start,fn); return
            if self.peek().kind==Kind.NAME and self.peek(1).kind in (Kind.NAME,Kind.STRING):
                raise Error(self.file,self.peek(1).span,f"function call {self.peek().text!r} requires parentheses")
        right=self.expect(Kind.NAME,Kind.STRING)
        r=Relation(left.text,op.text,right.text,Span(left.span.start,right.span.end))
        if self.mode=='where': self.type.where.append(r)
        elif self.mode=='input': self.type.inputs.append(r)
        else: self.type.outputs.append(r)
    def parse(self)->Program:
        while self.peek().kind!=Kind.EOF:
            self.separators()
            if self.peek().kind==Kind.EOF: break
            k=self.peek().kind
            if k==Kind.RPAR and self.segment_group==self.mode:
                self.take(); self.segment_group=None; continue
            if self.segment_group==self.mode and k in (Kind.UNIVERSE,Kind.USES,Kind.DEF,Kind.RETURN,Kind.WHERE,Kind.RUN,Kind.IMPORT):
                raise Error(self.file,self.peek().span,f"expected ')' to close {self.mode} segment")
            if k==Kind.UNIVERSE: self.open_universe()
            elif k==Kind.USES: self.open_uses()
            elif k==Kind.DEF: self.open_type()
            elif k==Kind.RETURN: self.open_return()
            elif k==Kind.WHERE: self.open_where()
            elif k==Kind.REDUCE:
                if self.mode!='where': raise Error(self.file,self.peek().span,"runaway 'reduce'")
                self.reduce_statement()
            elif k==Kind.DO: raise Error(self.file,self.peek().span,"'do' is not implemented")
            elif k==Kind.IMPORT:
                self.finish(); token=self.take(); n=self.expect(Kind.NAME,Kind.STRING); span=Span(token.span.start,n.span.end); self.program.imports.append((n.text,span)); self.importer and self.importer(self.program,n.text,span)
            elif k==Kind.RUN:
                self.finish(); token=self.take(); n=self.expect(Kind.NAME); self.program.runs.append(RunType(self.universe.name,n.text,Span(token.span.start,n.span.end),list(self.uses)))
            else: self.statement()
        if self.segment_group is not None: raise Error(self.file,self.peek().span,f"expected ')' to close {self.segment_group} segment")
        self.finish(); return self.program

def merge_program(dst:Program,src:Program)->None:
    for su in src.universes:
        if not su.types: continue
        du=next((u for u in dst.universes if u.name==su.name),None)
        if du is None: dst.universes.append(su)
        else:
            have={id(t) for t in du.types}; du.types += [t for t in su.types if id(t) not in have]
    have={id(r) for r in dst.runs}; dst.runs += [r for r in src.runs if id(r) not in have]

def load_program(file:str,source:str|None=None,seen:set[Path]|None=None,cache:dict[Path,Program]|None=None)->Program:
    path=Path(file).resolve(); seen=set() if seen is None else seen; cache={} if cache is None else cache
    if source is None and path in cache: return cache[path]
    if path in seen: return cache.get(path,Program())
    seen.add(path); source=path.read_text(encoding='utf-8') if source is None else source
    def importer(program:Program,name:str,span:Span)->None:
        raw=name[1:-1] if name.startswith('"') and name.endswith('"') else name.replace('.','/')+'.gs'
        child=(path.parent/raw).resolve(); imported=None
        try:
            imported=load_program(str(child),None,seen,cache); Resolver(str(child),imported).resolve(); merge_program(program,imported)
        except OSError as e: raise Error(str(path),span,f'cannot import {name!r}: {e}') from e
        except Error as e:
            merge_program(program,getattr(e,'program',None) or imported or Program()); e.program=program; raise
    parser=Parser(str(path),Lexer(str(path),source).tokens(),importer)
    try:
        program=parser.parse(); cache[path]=program; return program
    except Error as e:
        e.program=getattr(e,'program',None) or parser.partial_program(); cache[path]=e.program; raise

class Resolver:
    def __init__(self,file:str,program:Program): self.file,self.program=file,program
    def resolve(self)->None:
        # String and numeric literals are implicit atomic types in Impl.
        impl_u=next((u for u in self.program.universes if u.name=='Impl'),None)
        if impl_u is None:
            impl_u=Universe('Impl'); self.program.universes.append(impl_u)
        literals=set()
        for u in self.program.universes:
            for t in u.types:
                for ss in [t.inputs]+([t.outputs] if t.outputs is not None else []):
                    for x in ss:
                        if isinstance(x,Field): literals.update(n for n in x.types if '::' not in n and is_literal(n))
                        elif isinstance(x,Relation):
                            literals.update(n for n in (x.left,x.right) if is_literal(n))
                for w in t.where or []:
                    if isinstance(w,Relation): literals.update(n for n in (w.left,w.right) if is_literal(n))
                    else:
                        literals.update(n for n in w.args if is_literal(n))
                        for rel in w.relations: literals.update(n for n in (rel.left,rel.right) if is_literal(n))
        for n in sorted(literals):
            if not any(x.name==n for x in impl_u.types): impl_u.types.append(Type('Impl',n))

        types={}
        for u in self.program.universes:
            for t in u.types: types.setdefault(t.full,[]).append(t)

        def resolve_bare(name:str, universe:str, uses:list[str], span:Span, what:str, require_function:bool=False)->str:
            if '::' in name: return name
            if is_literal(name): return f'Impl::{name}'
            search=[universe]+list(uses)
            for un in search:
                q=f'{un}::{name}'
                if q in types and (not require_function or any(z.function for z in types[q])): return q
            detail=' -> '.join(search) or universe
            kind='function' if require_function else 'type'
            raise Error(self.file,span,f"unknown {kind} {name!r} while resolving {what}. Lookup priority was {detail}. Qualify the name explicitly with Universe::{name}, add a 'uses <Universe>' directive in this universe segment, or import the declaration")

        for r in self.program.runs:
            r.name=resolve_bare(r.name,r.universe,r.uses,r.span,'run target')
            if r.name not in types: raise Error(self.file,r.span,f"cannot run {r.name!r}: no declaration with that qualified name exists")
            runnable=[t for t in types[r.name] if not t.function or t.return_all]
            if not runnable:
                raise Error(self.file,r.span,f"cannot run {r.name!r}: every declaration with this name has an explicit return shape. 'run' needs either a data-type graph with no return clause or a 'return all' declaration")

        for u in self.program.universes:
            for t in u.types:
                for ss in [t.inputs]+([t.outputs] if t.outputs is not None else []):
                    for x in ss:
                        if not isinstance(x,Field): continue
                        x.types=[resolve_bare(n,t.universe,t.uses,x.span,f"type of variable {x.name!r}") for n in x.types]
                        for n in x.types:
                            if n not in types: raise Error(self.file,x.span,f"unknown type {n!r} used by variable {x.name!r}; lookup resolved the name but no declaration exists")
                for x in t.where or []:
                    if not isinstance(x,Reduce): continue
                    x.function=resolve_bare(x.function,t.universe,t.uses,x.span,f"call producing {x.ret!r}",True)
                    if x.function not in types or not any(z.function for z in types[x.function]):
                        raise Error(self.file,x.span,f"unknown function {x.function!r} used by reduction result {x.ret!r}; no function declaration with that qualified name exists")

@dataclass
class Node:
    id:int; types:frozenset[str]; names:dict[str,set[str]]=field(default_factory=dict); input_names:set[str]=field(default_factory=set)

@dataclass(frozen=True)
class Edge:
    left:int; tag:str; right:int

@dataclass
class Graph:
    nodes:dict[int,Node]=field(default_factory=dict); edges:list[Edge]=field(default_factory=list); next_id:int=0
    def add_node(self,types:frozenset[str],theory:str,name:str)->int:
        i=self.next_id; self.next_id+=1; self.nodes[i]=Node(i,types,{theory:{name}}); return i
    def lookup(self,name:str,theory:str|None=None)->int|None:
        for i,n in self.nodes.items():
            views=[n.names.get(theory,set())] if theory is not None else n.names.values()
            if any(name in names for names in views): return i
        return None
    def merge(self,a:int,b:int,span:Span|None=None,file:str='')->int:
        if a==b: return a
        x,y=self.nodes[a],self.nodes[b]
        if x.types!=y.types:
            if span: raise Error(file,span,f"cannot merge variables {best_name(x)!r} and {best_name(y)!r}: their type sets differ ({' | '.join(sorted(x.types)) or '<none>'} vs {' | '.join(sorted(y.types)) or '<none>'}). Identity merges require exactly compatible types")
            raise ValueError('incompatible node types')
        for theory,names in y.names.items(): x.names.setdefault(theory,set()).update(names)
        x.input_names.update(y.input_names)
        self.edges=list(dict.fromkeys(Edge(a if e.left==b else e.left,e.tag,a if e.right==b else e.right) for e in self.edges)); del self.nodes[b]; return a
    def induced(self,ids:set[int])->tuple[Graph,dict[int,int]]:
        g,remap=Graph(),{}
        for old in ids:
            n=self.nodes[old]; new=g.add_node(n.types,'',''); g.nodes[new].names={k:set(v) for k,v in n.names.items()}; g.nodes[new].input_names=set(n.input_names); remap[old]=new
        g.edges=[Edge(remap[e.left],e.tag,remap[e.right]) for e in self.edges if e.left in ids and e.right in ids]; return g,remap
    def remove_component(self,ids:set[int],protected:set[int],file:str,span:Span,within:set[int]|None=None)->None:
        remove=set(ids); within=set(self.nodes) if within is None else set(within)
        if remove&protected: raise Error(file,span,f"reduction cleanup would drop output node(s): {', '.join(best_name(self.nodes[i]) for i in sorted(remove&protected) if i in self.nodes)}",self.induced(set(self.nodes))[0])
        while True:
            add=set()
            for e in self.edges:
                if e.left in remove and e.right in within and e.right not in remove and e.right not in protected: add.add(e.right)
                if e.right in remove and e.left in within and e.left not in remove and e.left not in protected: add.add(e.left)
            if not add: break
            remove|=add
        if not set(self.nodes)-remove: raise Error(file,span,'reduction cleanup would remove the entire graph; at least one node must remain after removing the unused reduction component',self.induced(set(self.nodes))[0])
        self.edges=[e for e in self.edges if e.left not in remove and e.right not in remove]
        for i in remove: self.nodes.pop(i,None)

class Builder:
    def __init__(self,file:str,program:Program,selection:dict[str,Type]|None=None):
        self.file,self.program=file,program
        self.groups={}
        for u in program.universes:
            for t in u.types: self.groups.setdefault(t.full,[]).append(t)
        self.types=selection or {k:v[-1] for k,v in self.groups.items()}
    def qualify(self,p:str,n:str)->str: return f'{p}.{n}' if p else n
    def add_shape(self,g:Graph,t:Type,prefix:str,theory:str,output:bool=False,stack:tuple[str,...]=())->dict[str,int]:
        if t.full in stack: raise Error(self.file,t.span or Span(Pos(0,1,1),Pos(0,1,1)),f'recursive type {t.full}')
        ss,out=(t.outputs if output else t.inputs) or [],{}
        if not output and len(ss)==1 and isinstance(ss[0],Field) and ss[0].name=='$':
            x=ss[0]; target=x.types[0]; chosen=self.types.get(target)
            if chosen is None: raise Error(self.file,x.span,f'alias {t.full} refers to unknown type {target!r}')
            if not chosen.inputs and not chosen.function: out[prefix or t.name]=g.add_node(frozenset({target}),theory,prefix or t.name); return out
            return self.add_shape(g,chosen,prefix,theory,False,stack+(t.full,))
        for x in ss:
            if isinstance(x,Relation): continue
            atomic=[n for n in x.types if n in self.types and not self.types[n].inputs and not self.types[n].function]; structured=[n for n in x.types if n not in atomic]; name=self.qualify(prefix,x.name)
            if atomic: out[name]=g.add_node(frozenset(atomic),theory,name)
            for child in structured:
                if child not in self.types: raise Error(self.file,x.span,f'unknown type {child!r}')
                chosen=self.types[child]; out.update(self.add_shape(g,chosen,name,theory,chosen.outputs is not None,stack+(t.full,)))
        for x in ss:
            if not isinstance(x,Relation): continue
            a,b=self.qualify(prefix,x.left),self.qualify(prefix,x.right)
            if a not in out or b not in out: raise Error(self.file,x.span,f"cannot add relation {x.left!r} {x.tag} {x.right!r} in {t.full!r}: one or both endpoints do not resolve to leaf variables ({a!r}, {b!r}); available leaves: {', '.join(sorted(out)) or '<none>'}")
            g.edges.append(Edge(out[a],x.tag,out[b]))
        return out
    def function_input(self,t:Type)->tuple[Graph,dict[str,int]]:
        g=Graph(); return g,self.add_shape(g,t,'',t.universe)
    def build_function(self,t:Type)->tuple[Graph,dict[str,int],dict[str,int]]:
        # `return all` is deliberately a declaration flag.  We first build the
        # input graph, then execute the where/reduction graph, and only then
        # derive the return set from the surviving graph.
        if t.return_all:
            g=Graph(); ins=self.add_shape(g,t,'',t.universe)
            for name,gid in ins.items(): g.nodes[gid].input_names.add(name)
            visible_roots={name.split('.',1)[0] for name in ins}
            for x in t.where or []:
                if isinstance(x,Relation):
                    if x.tag!='=':
                        raise Error(self.file,x.span,
                            f"invalid where relation in {t.full!r}: {x.left!r} {x.tag} {x.right!r}. "
                            "A where-level relation merges variable identities and therefore must use '='; "
                            "pass non-equality relations as function-call relation arguments instead", g.induced(set(g.nodes))[0])
                    a,b=g.lookup(x.left),g.lookup(x.right)
                    if a is None or b is None:
                        missing=[n for n,i in ((x.left,a),(x.right,b)) if i is None]
                        available=', '.join(sorted({name for n in g.nodes.values() for names in n.names.values() for name in names if name})) or '<none>'
                        raise Error(self.file,x.span,
                            f"cannot merge where variables in {t.full!r}: {x.left!r} = {x.right!r}; "
                            f"unknown variable(s): {', '.join(repr(n) for n in missing)}. Available variables: {available}",
                            g.induced(set(g.nodes))[0])
                    keep=g.merge(a,b,x.span,self.file)
                else:
                    if x.function==t.full:
                        raise Error(self.file,x.span,
                            f"cannot infer 'return all' for recursive call {x.function!r} inside {t.full!r}; "
                            "the complete returned graph would depend on itself. Declare an explicit return shape for recursive functions",
                            g.induced(set(g.nodes))[0])
                    if not x.all and not x.ret.startswith('__tmp'):
                        visible_roots.add(x.ret.split('.',1)[0])
                    if x.all: self.apply_all(g,t,x)
                    else: self.apply_reduce(g,t,x)

            # Reductions are substitutions: they may consume part (or all) of an
            # originally declared input shape.  `return all` describes the complete
            # *surviving* post-where graph, so an input name that was consumed by a
            # substitution is no longer an input of the resulting graph.  Keep only
            # input aliases that still resolve after all where/reduce steps.
            refreshed={}
            for name in ins:
                gid=g.lookup(name,t.universe)
                if gid is not None:
                    refreshed[name]=gid
            ins=refreshed

            # Expose user variables (inputs and named where results) as return names.
            # Internal optimizer/call nodes remain part of the returned graph via the
            # return_all flag, but are intentionally not given public synthetic names.
            outs={}
            for i,n in g.nodes.items():
                for name in sorted(n.names.get(t.universe,set()),key=lambda z:(len(z),z)):
                    root=name.split('.',1)[0]
                    if root in visible_roots:
                        outs.setdefault(name,i)
            return g,ins,outs

        g=Graph(); ins=self.add_shape(g,t,'',t.universe); outs=self.add_shape(g,t,'',t.universe,True)
        for name,gid in ins.items(): g.nodes[gid].input_names.add(name)
        def merge_maps(a:int,b:int,span:Span)->int:
            nonlocal ins,outs
            keep=g.merge(a,b,span,self.file)
            ins={k:keep if v in (a,b) else v for k,v in ins.items()}
            outs={k:keep if v in (a,b) else v for k,v in outs.items()}
            return keep
        for name,a in list(ins.items()):
            if name in outs:
                merge_maps(a,outs[name],t.span or Span(Pos(0,1,1),Pos(0,1,1)))

        def bind_explicit_result(r:Reduce)->None:
            """Bind a call result namespace onto the explicitly declared return shape."""
            targets={**ins,**outs}
            target_names=[name for name in targets if name==r.ret or name.startswith(r.ret+'.')]
            if not target_names:
                raise Error(self.file,r.span,
                    f"call result {r.ret!r} from {r.function!r} has no location in the explicit return graph of {t.full!r}. "
                    f"Declared writable leaves: {', '.join(sorted(targets)) or '<none>'}. Declare a compatible input/return variable or use 'return all'",
                    g.induced(set(g.nodes))[0])

            # Structured results naturally use names such as r.child. Merge any
            # produced node carrying the same name into the predeclared return leaf.
            matched=set()
            for name in list(target_names):
                target=targets[name]
                ids=[i for i,n in g.nodes.items() if any(name in names for names in n.names.values())]
                for other in ids:
                    if other==target or other not in g.nodes: continue
                    if g.nodes[target].types!=g.nodes[other].types:
                        raise Error(self.file,r.span,
                            f"call {r.function!r} produced variable {name!r} with type(s) {sorted(g.nodes[other].types)}, but the explicit return variable {name!r} in {t.full!r} has type(s) {sorted(g.nodes[target].types)}",
                            g.induced(set(g.nodes))[0])
                    target=merge_maps(target,other,r.span)
                    if name in outs: outs[name]=target
                    if name in ins: ins[name]=target
                    targets[name]=target; matched.add(name)

            # Scalar shorthand: `return r:T` can receive a one-leaf function result
            # whose internal output name is namespaced as r.<callee-output>.
            if r.ret in targets and r.ret not in matched:
                target=targets[r.ret]
                produced=[]
                for i,n in g.nodes.items():
                    if i==target: continue
                    aliases_here={a for names in n.names.values() for a in names}
                    if any(a.startswith(r.ret+'.') for a in aliases_here): produced.append(i)
                produced=list(dict.fromkeys(produced))
                compatible=[i for i in produced if g.nodes[i].types==g.nodes[target].types]
                if len(compatible)==1:
                    bound=merge_maps(target,compatible[0],r.span)
                    if r.ret in outs: outs[r.ret]=bound
                    if r.ret in ins: ins[r.ret]=bound
                    targets[r.ret]=bound; matched.add(r.ret)
                elif produced:
                    details='; '.join(f"{best_name(g.nodes[i])}: {' | '.join(sorted(g.nodes[i].types))}" for i in produced if i in g.nodes)
                    raise Error(self.file,r.span,
                        f"cannot place result {r.ret!r} of {r.function!r} into explicit return variable {r.ret!r}. The return variable expects type(s) {' | '.join(sorted(g.nodes[target].types))}, but the call produced {len(produced)} candidate leaf/leaves: {details}. Use a structured return shape matching the callee output, or use 'return all'",
                        g.induced(set(g.nodes))[0])

        for x in t.where or []:
            if isinstance(x,Relation):
                if x.tag!='=':
                    raise Error(self.file,x.span,f"invalid function where relation in {t.full!r}: {x.left!r} {x.tag} {x.right!r}; function where-relations merge identities and must use '='. Non-identity relations belong in the declared input/return shape or as relation arguments to a call")
                if is_literal(x.left): self.materialize_literal(g,t,x.left,x.span)
                if is_literal(x.right): self.materialize_literal(g,t,x.right,x.span)
                a,b=g.lookup(x.left),g.lookup(x.right)
                if a is None or b is None:
                    missing=[n for n,i in ((x.left,a),(x.right,b)) if i is None]
                    available=', '.join(sorted({name for n in g.nodes.values() for names in n.names.values() for name in names if name})) or '<none>'
                    raise Error(self.file,x.span,f"unknown where variable(s) in {t.full!r}: {', '.join(repr(n) for n in missing)} while applying relation {x.left!r} = {x.right!r}. Available variables: {available}",g.induced(set(g.nodes))[0])
                merge_maps(a,b,x.span)
            else:
                if x.all:
                    self.apply_all(g,t,x)
                    # Optimizers may merge or replace declared leaves; refresh maps by name.
                    for name in list(ins):
                        gid=g.lookup(name,t.universe)
                        if gid is not None: ins[name]=gid
                    for name in list(outs):
                        gid=g.lookup(name,t.universe)
                        if gid is not None: outs[name]=gid
                else:
                    self.apply_reduce(g,t,x)
                    bind_explicit_result(x)
        return g,ins,outs

    def _declared_types_for_name(self,t:Type,name:str)->set[str]:
        root=name.split('.',1)[0]; out=set()
        for x in t.inputs:
            if isinstance(x,Field) and x.name==root: out.update(x.types)
        return out
    def build_type(self,t:Type)->Graph:
        g=Graph(); self.add_shape(g,t,'',t.universe)
        for x in t.where or []:
            if isinstance(x,Relation):
                if x.tag!='=': raise Error(self.file,x.span,f"invalid where relation {x.left!r} {x.tag} {x.right!r} in {t.full!r}: where-level relations merge variable identities and therefore must use '='")
                a,b=g.lookup(x.left),g.lookup(x.right)
                if a is None or b is None: raise Error(self.file,x.span,f"cannot apply where merge {x.left!r} = {x.right!r} in {t.full!r}: one or both variables are absent from the current graph",g.induced(set(g.nodes))[0])
                g.merge(a,b,x.span,self.file)
            else:
                if x.all: self.apply_all(g,t,x)
                else: self.apply_reduce(g,t,x)
        return g
    def materialize_literal(self,g:Graph,owner:Type,value:str,span:Span)->int:
        existing=g.lookup(value)
        if existing is not None: return existing
        if not is_literal(value):
            raise Error(self.file,span,f"internal error: {value!r} was requested as a literal but is not a string or numeric literal")
        if not owner.return_all:
            raise Error(self.file,span,
                f"literal {value!r} used inside the where block of {owner.full!r} needs an implicit temporary variable of type Impl::{value}. "
                "Implicit where temporaries are only allowed with 'return all'; either enable 'return all' or declare/pass an existing variable instead")
        name=f'__literal{g.next_id}'
        i=g.add_node(frozenset({f'Impl::{value}'}),owner.universe,name)
        g.nodes[i].names.setdefault(owner.universe,set()).add(value)
        return i

    def materialize_reduce_literals(self,g:Graph,owner:Type,r:Reduce)->None:
        values=set(x for x in r.args if is_literal(x))
        for rel in r.relations:
            values.update(x for x in (rel.left,rel.right) if is_literal(x))
        for value in values: self.materialize_literal(g,owner,value,r.span)

    def named_members(self,g:Graph,name:str,span:Span,role:str='variable')->set[int]:
        """Resolve a graph name as either an exact node or a structured prefix.

        Unlike expand_args(), this deliberately does *not* collapse a structured call
        name to its returned value.  It is used by relation arguments, where `r rel y`
        means the relation applies to every currently materialized member of `r` and
        every member of `y`.
        """
        exact={i for i,n in g.nodes.items() for names in n.names.values() if name in names}
        prefixed={i for i,n in g.nodes.items() for names in n.names.values() if any(x.startswith(name+'.') for x in names)}
        matches=exact|prefixed
        if not matches:
            available=', '.join(sorted({x for n in g.nodes.values() for names in n.names.values() for x in names if x})) or '<none>'
            raise Error(self.file,span,
                f"call relation {role} {name!r} does not resolve to an existing graph variable or structured-variable prefix. "
                f"Available variables: {available}. A relation may only mention variables already declared as inputs/outputs or introduced by an earlier assignment under 'return all'.",
                g.induced(set(g.nodes))[0])
        return matches
    def expand_args(self,g:Graph,args:list[str],span:Span)->set[int]:
        out=set()
        for arg in args:
            matches=self.named_members(g,arg,span,'argument')
            returned={e.right for e in g.edges if e.tag=='returns' and e.left in matches and e.right in matches}
            if returned: matches=returned
            out|=matches
        return out
    def edge_sig(self,g:Graph,a:int,b:int)->tuple[str,...]: return tuple(sorted(e.tag for e in g.edges if e.left==a and e.right==b and not e.tag.startswith('!')))
    def negative_sig(self,g:Graph,a:int,b:int)->tuple[str,...]: return tuple(sorted(e.tag[1:] for e in g.edges if e.left==a and e.right==b and e.tag.startswith('!')))
    def compatible_edges(self,a:Graph,b:Graph,x:int,xx:int,y:int,yy:int,strict:bool=False)->bool:
        expected,actual=set(self.edge_sig(a,x,xx)),set(self.edge_sig(b,y,yy))
        # Positive relations in a function input are preconditions, not facts that a
        # call may invent after matching.  The caller may contain additional positive
        # relations; those do not invalidate the match.  Negative relations remain
        # hard exclusions.  `strict` is retained for callers/API compatibility.
        if not expected<=actual: return False
        return not set(self.negative_sig(a,x,xx))&actual
    def subgraph_isomorphism(self,a:Graph,b:Graph)->dict[int,int]|None:
        candidates={x:[y for y in b.nodes if a.nodes[x].types==b.nodes[y].types] for x in a.nodes}
        if any(not x for x in candidates.values()): return None
        order=sorted(a.nodes,key=lambda x:(len(candidates[x]),-sum(e.left==x or e.right==x for e in a.edges)))
        def search(i:int,m:dict[int,int],used:set[int])->dict[int,int]|None:
            if i==len(order): return m.copy()
            x=order[i]
            for y in candidates[x]:
                if y in used or any(not self.compatible_edges(a,b,x,xx,y,yy,True) or not self.compatible_edges(a,b,xx,x,yy,y,True) for xx,yy in m.items()): continue
                m[x]=y; used.add(y); r=search(i+1,m,used)
                if r is not None: return r
                used.remove(y); del m[x]
            return None
        return search(0,{},set())
    def isomorphism(self,a:Graph,b:Graph,allowed:dict[int,set[int]]|None=None)->dict[int,int]|None:
        if len(a.nodes)!=len(b.nodes): return None
        candidates={x:[y for y in b.nodes if a.nodes[x].types==b.nodes[y].types and (allowed is None or x not in allowed or y in allowed[x])] for x in a.nodes}
        if any(not x for x in candidates.values()): return None
        order=sorted(a.nodes,key=lambda x:(len(candidates[x]),-sum(e.left==x or e.right==x for e in a.edges)))
        def search(i:int,m:dict[int,int],used:set[int])->dict[int,int]|None:
            if i==len(order): return m.copy()
            x=order[i]
            for y in candidates[x]:
                if y in used or any(not self.compatible_edges(a,b,x,xx,y,yy) or not self.compatible_edges(a,b,xx,x,yy,y) for xx,yy in m.items()): continue
                m[x]=y; used.add(y); r=search(i+1,m,used)
                if r is not None: return r
                used.remove(y); del m[x]
            return None
        return search(0,{},set())
    def mismatch(self,expected:Graph,actual:Graph)->str:
        lines=[]
        if len(expected.nodes)!=len(actual.nodes): lines.append(f'node count: expected {len(expected.nodes)}, got {len(actual.nodes)}')
        et=[sorted(n.types) for n in expected.nodes.values()]; at=[sorted(n.types) for n in actual.nodes.values()]
        for types in sorted({tuple(x) for x in et}):
            a=et.count(list(types)); b=at.count(list(types))
            if a!=b: lines.append(f'node type {" | ".join(types)}: expected {a}, got {b}')
        emap={}; used=set()
        for x in expected.nodes:
            ys=[y for y in actual.nodes if y not in used and expected.nodes[x].types==actual.nodes[y].types]
            if ys: emap[x]=ys[0]; used.add(ys[0])
        def edge_name(g:Graph,i:int)->str: return best_name(g.nodes[i])
        for e in expected.edges:
            if not e.tag.startswith('!') or e.left not in emap or e.right not in emap: continue
            tag=e.tag[1:]; a,b=emap[e.left],emap[e.right]
            if any(x.left==a and x.right==b and x.tag==tag for x in actual.edges): lines.append(f'forbidden relation present: {edge_name(actual,a)} {tag} {edge_name(actual,b)}')
        return '; '.join(lines) or 'node identities/relations cannot be mapped consistently'
    
    def apply_reduce(self,g:Graph,owner:Type,r:Reduce,selected_override:set[int]|None=None,match_override:tuple[Type,dict[int,int]]|None=None)->None:
        if selected_override is None: self.materialize_reduce_literals(g,owner,r)
        before=g.induced(set(g.nodes))[0]
        if r.function not in self.groups: raise Error(self.file,r.span,f'unknown function {r.function!r}; no declaration with that qualified name exists',before)
        call_relation_edges=[]
        if selected_override is None:
            # A relation argument is a caller-side assertion over structured names.
            # `r followedby y` therefore expands to the Cartesian product of all
            # currently materialized members of r and y.  Matching still selects call
            # value arguments normally (e.g. r collapses to r.result), so the declared
            # callee relation is checked specifically on the mapped argument values.
            for rel in r.relations:
                lefts=self.named_members(g,rel.left,rel.span,'left endpoint')
                rights=self.named_members(g,rel.right,rel.span,'right endpoint')
                for a in lefts:
                    for b in rights:
                        edge=Edge(a,rel.tag,b)
                        if edge not in g.edges: g.edges.append(edge)
                        call_relation_edges.append(edge)
        selected=self.expand_args(g,r.args,r.span) if selected_override is None else set(selected_override)
        actual,actual_map=g.induced(selected); valid=[]; rejected=[]
        for ai,fn in enumerate([x for x in self.groups[r.function] if x.function],1):
            if match_override is not None and fn is not match_override[0]: continue
            sel=dict(self.types); sel[r.function]=fn; cb=Builder(self.file,self.program,sel); template,inputs,outputs=cb.build_function(fn); expected,expected_map=cb.function_input(fn); allowed=None
            fields=[x for x in fn.inputs if isinstance(x,Field)]
            if match_override is not None:
                raw=match_override[1]; match={eid:actual_map[gid] for eid,gid in raw.items() if gid in actual_map}
                if len(match)==len(expected.nodes): valid.append((fn,template,inputs,outputs,expected,expected_map,match,cb))
                continue
            if selected_override is None and len(fields)==len(r.args):
                allowed={}
                for field,arg in zip(fields,r.args):
                    aids={actual_map[x] for x in self.expand_args(g,[arg],r.span) if x in actual_map}; prefix=field.name+'.'
                    for name,eid in expected_map.items():
                        if name==field.name or name.startswith(prefix): allowed[eid]=aids
            match=cb.isomorphism(expected,actual,allowed)
            if match is not None and r.relations:
                # Relation arguments must correspond to relations declared by the callee
                # between the mapped value arguments.  The graph may contain the expanded
                # Cartesian-product edges as useful caller facts, but only the value-to-value
                # edge participates in the callee contract check.
                inv={aid:eid for eid,aid in match.items()}
                missing=[]
                for rel in r.relations:
                    lvals=self.expand_args(g,[rel.left],rel.span)
                    rvals=self.expand_args(g,[rel.right],rel.span)
                    ok=False
                    for lgid in lvals:
                        for rgid in rvals:
                            al,ar=actual_map.get(lgid),actual_map.get(rgid)
                            el,er=inv.get(al),inv.get(ar)
                            if el is not None and er is not None and any(e.left==el and e.tag==rel.tag and e.right==er for e in expected.edges):
                                ok=True; break
                        if ok: break
                    if not ok:
                        missing.append(f'{rel.left} {rel.tag} {rel.right}')
                if missing:
                    declared=', '.join(f'{best_name(expected.nodes[e.left])} {e.tag} {best_name(expected.nodes[e.right])}' for e in expected.edges if not e.tag.startswith('!')) or '<none>'
                    rejected.append(f"alternative {ai}: call supplies relation(s) {', '.join(missing)}, but the function input does not declare the corresponding relation between the mapped arguments. Declared positive input relations: {declared}")
                    match=None
            if match is not None: valid.append((fn,template,inputs,outputs,expected,expected_map,match,cb))
            elif not rejected or not (rejected[-1].startswith(f'alternative {ai}:') and ('function input does not declare' in rejected[-1] or 'call supplies relation(s)' in rejected[-1])):
                detail=cb.mismatch(expected,actual)
                # Give a contract-oriented explanation when positional/field binding
                # identifies the endpoints of a required input relation but the caller
                # has not supplied that edge.  This is much more useful than a generic
                # isomorphism failure for declarations such as `a followedby b`.
                missing_required=[]
                if allowed is not None:
                    rev_expected={eid:name for name,eid in expected_map.items()}
                    for ee in expected.edges:
                        if ee.tag.startswith('!'): continue
                        ls=allowed.get(ee.left,set()); rs=allowed.get(ee.right,set())
                        if not ls or not rs: continue
                        if not any(any(x.left==l and x.tag==ee.tag and x.right==rr for x in actual.edges) for l in ls for rr in rs):
                            missing_required.append(f"{rev_expected.get(ee.left,best_name(expected.nodes[ee.left]))} {ee.tag} {rev_expected.get(ee.right,best_name(expected.nodes[ee.right]))}")
                if missing_required:
                    detail=(f"missing required function-input relation(s): {', '.join(missing_required)}. "
                            f"The declaration of {fn.full!r} requires these relations to already hold between the mapped call arguments; "
                            "pass the corresponding relation argument in the call or establish it in the caller graph before the call")
                rejected.append(f'alternative {ai}: {detail}')
        if not valid: raise Error(self.file,r.span,f"cannot assign reduction result {r.ret!r} from function {r.function!r}: none of its declared input graphs matches the selected call arguments. The alternatives were rejected for these node/type/relation differences:\n"+'\n'.join(rejected),before)
        if len(valid)>1: raise Error(self.file,r.span,f"ambiguous reduction for result {r.ret!r}: {len(valid)} declarations of {r.function!r} accept the same selected variables/types/relations. Make their input graphs structurally distinguishable",before)
        fn,template,inputs,outputs,expected,expected_map,match,cb=valid[0]; actual_back={v:k for k,v in actual_map.items()}; output_ids=(set(template.nodes) if fn.return_all else set(outputs.values()))
        concrete={eid:actual_back[match[eid]] for eid in expected.nodes}; matched_original=set(concrete.values())
        input_edges={Edge(concrete[e.left],e.tag,concrete[e.right]) for e in expected.edges if not e.tag.startswith('!')}
        eid_tid={eid:inputs[name] for name,eid in expected_map.items()}; groups={}
        for name,eid in expected_map.items(): groups.setdefault(inputs[name],[]).append(concrete[eid])

        # A reduce-all match may cover only part of a variadic call's arguments.
        # Such a call is residual context rather than a node to delete. Lift the
        # replacement output through that call: keep its surviving operand edge
        # and returns edge, and use its result as the replacement representative.
        promoted={}; promoted_operands={}; residual_calls=set(); preserve_edges=set()
        if selected_override is not None:
            for ceid,cgid in concrete.items():
                tid=eid_tid.get(ceid)
                if tid in output_ids or call_name(g.nodes[cgid]) is None: continue
                extra=[e for e in g.edges if e.tag=='arg' and e.right==cgid and e not in input_edges and e.left not in matched_original]
                if not extra: continue
                rets=[e for e in expected.edges if not e.tag.startswith('!') and e.left==ceid and e.tag=='returns']
                if len(rets)!=1: raise Error(self.file,r.span,f'reduction {r.ret!r} cannot preserve variadic context around {best_name(g.nodes[cgid])!r}: the matched call needs exactly one returns edge',before)
                rgid=concrete[rets[0].right]
                args=[e for e in expected.edges if not e.tag.startswith('!') and e.right==ceid and e.tag=='arg']
                tids={eid_tid[e.left] for e in args if eid_tid.get(e.left) in output_ids}
                if len(tids)!=1: raise Error(self.file,r.span,f'reduction {r.ret!r} cannot lift variadic context around {best_name(g.nodes[cgid])!r}: exactly one returned input identity must remain in the output',before)
                ptid=next(iter(tids))
                if ptid in promoted and promoted[ptid][0]!=rgid: raise Error(self.file,r.span,f'reduction {r.ret!r} has conflicting residual variadic outputs',before)
                ops={concrete[e.left] for e in args if eid_tid.get(e.left)==ptid}
                promoted[ptid]=(rgid,cgid); promoted_operands.setdefault(ptid,set()).update(ops); residual_calls.add(cgid)
                preserve_edges.add(Edge(cgid,'returns',rgid)); preserve_edges|={Edge(x,'arg',cgid) for x in ops}

        if selected_override is not None:
            consumed=input_edges-preserve_edges; g.edges=[e for e in g.edges if e not in consumed]

        redirect={i:i for i in matched_original}
        def resolve(i:int)->int:
            while redirect.get(i,i)!=i: i=redirect[i]
            return i
        def merge_into(a:int,b:int)->int:
            a,b=resolve(a),resolve(b)
            if a==b: return a
            g.merge(a,b,r.span,self.file)
            for k,v in list(redirect.items()):
                if resolve(v)==b or v==b: redirect[k]=a
            redirect[b]=a; selected.discard(b); selected.add(a)
            return a

        mapping={}; operand_keep={x for xs in promoted_operands.values() for x in xs}
        for tid,gids in groups.items():
            gids=list(dict.fromkeys(gids))
            if tid in promoted:
                gid=resolve(promoted[tid][0])
                for other in gids:
                    other=resolve(other)
                    if other==gid or other in {resolve(x) for x in promoted_operands.get(tid,set())}: continue
                    gid=merge_into(gid,other)
                mapping[tid]=gid
            else:
                live=[resolve(x) for x in gids if resolve(x) in g.nodes]
                if not live: continue
                gid=live[0]
                for other in live[1:]: gid=merge_into(gid,other)
                mapping[tid]=gid
        for tid,(rgid,cgid) in list(promoted.items()): promoted[tid]=(resolve(rgid),resolve(cgid)); mapping[tid]=resolve(rgid)

        # Do not leak callee-local aliases onto caller nodes for ordinary
        # substitutions. Public output aliases are added below with the call-result
        # prefix (for example `r.p.a`). A return-all callee is different: its whole
        # graph is intentionally exposed, so retain its internal aliases as metadata.
        if fn.return_all:
            for tid,gid in mapping.items():
                if gid not in g.nodes: continue
                for theory,names in template.nodes[tid].names.items():
                    g.nodes[gid].names.setdefault(theory,set()).update(names)

        created={}
        for name,tid in outputs.items():
            full=f'{r.ret}.{name}'
            if tid in mapping: gid=mapping[tid]
            elif tid in created: gid=created[tid]
            else: gid=g.add_node(template.nodes[tid].types,owner.universe,full); created[tid]=gid
            g.nodes[gid].names.setdefault(owner.universe,set()).add(full); created[tid]=gid
        allmap=mapping|created
        if fn.return_all:
            # `return all` returns the complete surviving function graph, not just
            # its publicly named variables. Materialize any internal graph nodes
            # anonymously so optimizer/call structure is preserved without leaking
            # synthetic user-visible variable names.
            for tid,node in template.nodes.items():
                if tid not in allmap:
                    allmap[tid]=g.add_node(node.types,owner.universe,'')
                    g.nodes[allmap[tid]].names={}
                for theory,names in node.names.items():
                    g.nodes[allmap[tid]].names.setdefault(theory,set()).update(names)

        # Ordinary calls assert/materialize their positive input relations; an
        # automatic rewrite consumes its matched input instead.
        if selected_override is None:
            rev={i:name for name,i in expected_map.items()}
            for e in expected.edges:
                if e.tag.startswith('!'): continue
                edge=Edge(allmap[inputs[rev[e.left]]],e.tag,allmap[inputs[rev[e.right]]])
                if edge not in g.edges: g.edges.append(edge)

        if fn.return_all:
            # The full post-where template is the return graph. Copy all positive
            # relations so callers receive exactly the graph that survived the function.
            for e in template.edges:
                if e.tag.startswith('!') or e.left not in allmap or e.right not in allmap: continue
                edge=Edge(allmap[e.left],e.tag,allmap[e.right])
                if edge not in g.edges: g.edges.append(edge)
        else:
            outg=Graph(); outnames=cb.add_shape(outg,fn,'',fn.universe,True); outrev={i:outputs[name] for name,i in outnames.items()}
            for e in outg.edges:
                edge=Edge(allmap[outrev[e.left]],e.tag,allmap[outrev[e.right]])
                if edge not in g.edges: g.edges.append(edge)

        matched={resolve(i) for i in matched_original if resolve(i) in g.nodes}
        protected={allmap[tid] for tid in output_ids if tid in allmap}

        if selected_override is not None:
            protected|={resolve(i) for i in residual_calls if resolve(i) in g.nodes}
            protected|={resolve(i) for i in operand_keep if resolve(i) in g.nodes}
            # Automatic rewrites can match inside a larger expression.  A matched
            # node that still has context outside the match is an anchor for that
            # surrounding expression and must survive.
            for e in g.edges:
                if e.tag.startswith('!'): continue
                if e.left in matched and e.right not in matched: protected.add(e.left)
                if e.right in matched and e.left not in matched: protected.add(e.right)
        else:
            # An ordinary function application is a true substitution of its
            # selected argument graph.  Once the function output has been
            # materialized, matched input nodes survive *only* when the declared
            # output graph reuses their identity.  This is what makes projections
            # and destructuring functions actually remove the discarded siblings
            # instead of leaving stale nodes in the caller.
            #
            # Do not treat arbitrary external edges as protection here: those edges
            # belonged to the consumed value.  A function that needs to retain an
            # input identity must expose it in its return graph.
            pass

        remove=matched-protected
        if remove:
            g.remove_component(remove,protected,self.file,r.span,matched)

        lost=[name for name,tid in outputs.items() if allmap.get(tid) not in g.nodes]
        if lost: raise Error(self.file,r.span,f"reduction {r.ret!r} would drop output node(s): {', '.join(lost)}",before)

    def graph_shape(self,g:Graph)->tuple:
        ns=tuple(sorted(tuple(sorted(n.types)) for n in g.nodes.values()))
        es=tuple(sorted((tuple(sorted(g.nodes[e.left].types)),e.tag,tuple(sorted(g.nodes[e.right].types))) for e in g.edges))
        return ns,es
    def apply_all(self,g:Graph,owner:Type,r:Reduce)->None:
        before_seen=set()
        while True:
            shape=self.graph_shape(g)
            if shape in before_seen: return
            before_seen.add(shape); choices=[]
            for fn in [x for x in self.groups.get(r.function,[]) if x.function]:
                sel=dict(self.types); sel[r.function]=fn; cb=Builder(self.file,self.program,sel); expected,_=cb.function_input(fn); match=cb.subgraph_isomorphism(expected,g)
                if match is not None:
                    rarity=min((sum(g.nodes[y].types==expected.nodes[x].types for y in g.nodes),x) for x in expected.nodes)[0]
                    choices.append((rarity,fn,match))
            if not choices: return
            choices.sort(key=lambda x:x[0]); _,fn,match=choices[0]; tmp=Reduce(f'{r.ret}{len(before_seen)-1}',r.function,[],r.span)
            self.apply_reduce(g,owner,tmp,set(match.values()),(fn,match))

def selections(program:Program)->list[dict[str,Type]]:
    groups={}
    for u in program.universes:
        for t in u.types: groups.setdefault(t.full,[]).append(t)
    items=list(groups.items()); out=[]
    def walk(i:int,cur:dict[str,Type])->None:
        if i==len(items): out.append(cur.copy()); return
        name,alts=items[i]
        for t in alts: cur[name]=t; walk(i+1,cur)
    walk(0,{})
    return out

def build_variants(file:str,program:Program,t:Type)->list[Graph]:
    groups={}
    for u in program.universes:
        for d in u.types: groups.setdefault(d.full,[]).append(d)
    def expand(d:Type,prefix:str='',stack:tuple[str,...]=())->list[Graph]:
        if d.full in stack: raise Error(file,d.span or Span(Pos(0,1,1),Pos(0,1,1)),f'recursive expansion through {d.full}')
        ss=(d.outputs if d.outputs is not None else d.inputs) or []
        if len(ss)==1 and isinstance(ss[0],Field) and ss[0].name=='$':
            ref=ss[0].types[0]; out=[]
            for alt in groups.get(ref,[]):
                if not alt.inputs and not alt.function:
                    g=Graph(); g.add_node(frozenset({ref}),d.universe,prefix or d.name); out.append(g)
                else: out+=expand(alt,prefix,stack+(d.full,))
            return out
        variants=[Graph()]
        for x in [z for z in ss if isinstance(z,Field)]:
            name=f'{prefix}.{x.name}' if prefix else x.name; choices=[Graph()]; multi=[r for r in x.types if len(groups.get(r,[]))>1]
            if multi:
                choices=[]
                import itertools
                pools=[groups[r] if len(groups.get(r,[]))>1 else [None] for r in x.types]
                for picked in itertools.product(*pools):
                    atomic=[]; structured=[]
                    for ref,alt in zip(x.types,picked):
                        dref=alt or (groups.get(ref) or [None])[-1]
                        if dref is None: raise Error(file,x.span,f'unknown type {ref!r}')
                        if not dref.inputs and not dref.function: atomic.append(ref)
                        else: structured.append(dref)
                    bases=[Graph()]
                    if atomic:
                        q=Graph(); q.add_node(frozenset(atomic),d.universe,name); bases=[q]
                    for child in structured:
                        nxt=[]
                        for base in bases:
                            for cg in expand(child,name,stack+(d.full,)):
                                ng=merge_graphs(base,cg); nxt.append(ng)
                        bases=nxt
                    choices+=bases
            else:
                atomic=[]; structured=[]
                for ref in x.types:
                    dref=(groups.get(ref) or [None])[-1]
                    if dref is None: raise Error(file,x.span,f'unknown type {ref!r}')
                    if not dref.inputs and not dref.function: atomic.append(ref)
                    else: structured.append(dref)
                choices=[]
                bases=[Graph()]
                if atomic:
                    q=Graph(); q.add_node(frozenset(atomic),d.universe,name); bases=[q]
                for child in structured:
                    nxt=[]
                    for base in bases:
                        for cg in expand(child,name,stack+(d.full,)): nxt.append(merge_graphs(base,cg))
                    bases=nxt
                choices=bases
            variants=[merge_graphs(a,b) for a in variants for b in choices]
        for g in variants:
            for r in [z for z in ss if isinstance(z,Relation)]:
                left=f'{prefix}.{r.left}' if prefix else r.left; right=f'{prefix}.{r.right}' if prefix else r.right; a=g.lookup(left); b=g.lookup(right)
                if a is None or b is None: raise Error(file,r.span,f'relation {r.left} {r.tag} {r.right} refers to a name that does not resolve to a leaf in this alternative')
                g.edges.append(Edge(a,r.tag,b))
        return variants
    def apply_where(g:Graph)->Graph:
        b=Builder(file,program)
        for n in g.nodes.values(): n.input_names.update(name for names in n.names.values() for name in names if name)
        for w in t.where or []:
            if isinstance(w,Relation):
                if w.tag!='=': raise Error(file,w.span,f"where relation {w.left} {w.tag} {w.right} is invalid: where relations merge identities and therefore must use '='")
                a,bid=g.lookup(w.left),g.lookup(w.right)
                if a is None or bid is None: raise Error(file,w.span,f'where merge {w.left} = {w.right} cannot be resolved in this alternative; one or both names are absent')
                g.merge(a,bid,w.span,file)
            else:
                if w.all: b.apply_all(g,t,w)
                else: b.apply_reduce(g,t,w)
        return g
    out=[apply_where(g) for g in expand(t)]
    uniq=[]; seen=set()
    for g in out:
        sig=(tuple(sorted((best_name(n),tuple(sorted(n.types))) for n in g.nodes.values())),tuple(sorted((best_name(g.nodes[e.left]),e.tag,best_name(g.nodes[e.right])) for e in g.edges)))
        if sig not in seen: seen.add(sig); uniq.append(g)
    return uniq

def merge_graphs(a:Graph,b:Graph)->Graph:
    g=Graph(); maps=[]
    for src in (a,b):
        m={}
        for old,n in src.nodes.items():
            i=g.add_node(n.types,'',''); g.nodes[i].names={k:set(v) for k,v in n.names.items()}; g.nodes[i].input_names=set(n.input_names); m[old]=i
        g.edges += [Edge(m[e.left],e.tag,m[e.right]) for e in src.edges]; maps.append(m)
    return g

def graph_diagnostic(g:Graph)->str:
    names=graph_names(g) if g.nodes else {}
    vars=[]
    for i in sorted(g.nodes,key=lambda j:names.get(j,f'#{j}')):
        n=g.nodes[i]; vars.append(f"{names.get(i,f'#{i}')} : {' | '.join(sorted(n.types)) or '<untyped>'}")
    rels=[f"{names.get(e.left,f'#{e.left}')} {e.tag} {names.get(e.right,f'#{e.right}')}" for e in g.edges]
    return "variables/types: " + (", ".join(vars) if vars else "<none>") + "\nrelations: " + (", ".join(rels) if rels else "<none>")

def aliases(n:Node)->list[str]: return sorted({name for names in n.names.values() for name in names if name},key=lambda x:(len(x),x))
def best_name(n:Node)->str:
    names=sorted(n.input_names,key=lambda x:(len(x),x)) or aliases(n)
    return names[0] if names else f'#{n.id}'
def graph_names(g:Graph,ids:set[int]|None=None)->dict[int,str]:
    ids=set(g.nodes) if ids is None else set(ids); candidates={}
    for i in ids:
        n=g.nodes[i]; xs=sorted(n.input_names,key=lambda x:(len(x),x)) or aliases(n); candidates[i]=xs
    counts={x:sum(x in xs for xs in candidates.values()) for xs0 in candidates.values() for x in xs0}
    return {i:(next((x for x in xs if counts[x]==1),xs[0] if xs else f'#{i}')) for i,xs in candidates.items()}
def aliasfmt(n:Node,first:str|None=None)->str:
    xs=aliases(n)
    if first in xs: xs=[first]+[x for x in xs if x!=first]
    return (' '+gray('=')+' ').join(cyan(x) for x in xs) if xs else gray(f'#{n.id}')
def print_node(n:Node,indent:int=0,name:str|None=None)->None: print(' '*indent+f"{cyan(name or best_name(n))} {gray(':')} {gray(' | ').join(typefmt(x) for x in sorted(n.types))}")
def print_edge(g:Graph,e:Edge,indent:int=0,names:dict[int,str]|None=None)->None:
    names=names or graph_names(g); print(' '*indent+f'{cyan(names[e.left])} {C.BOLD}{e.tag}{C.RESET} {cyan(names[e.right])}')
def print_graph(g:Graph,indent:int=0)->None:
    if not g.nodes: print(' '*indent+gray('∅')); return
    names=graph_names(g)
    for i in sorted(g.nodes,key=lambda i:names[i]): print_node(g.nodes[i],indent,names[i])
    for e in g.edges: print_edge(g,e,indent,names)
def print_subgraph(g:Graph,ids:set[int],indent:int=0)->None:
    if not ids: print(' '*indent+gray('∅')); return
    names=graph_names(g,ids)
    for i in sorted(ids,key=lambda i:names[i]): print_node(g.nodes[i],indent,names[i])
    for e in g.edges:
        if e.left in ids and e.right in ids: print_edge(g,e,indent,names)

TYPE_CONVERTERS={"Impl::Nat":int,"Impl::Int":int,"Impl::Real":float,"Impl::Float":float,"Impl::String":str,"Impl::nat":int,"Impl::int":int,"Impl::real":float,"Impl::float":float,"Impl::string":str}
IMPL_FUNCTIONS={}

def impl(name):
    def register(fn):
        IMPL_FUNCTIONS[f'Impl::{name}']=fn
        return fn
    return register

@impl('add')
def impl_add(xs):
    assert len(xs)>=1
    return sum(v for _,v in xs)

@impl('mul')
def impl_mul(xs):
    assert len(xs)>=1
    return __import__('math').prod(v for _,v in xs)

@impl('sub')
def impl_sub(xs):
    assert len(xs)==2
    first=[v for k,v in xs if k=='arg0']
    second=[v for k,v in xs if k=='arg1']
    assert len(first)==1 and len(second)==1
    return first[0]-second[0]

@impl('div')
def impl_div(xs):
    assert len(xs)==2
    first=[v for k,v in xs if k=='arg0']
    second=[v for k,v in xs if k=='arg1']
    assert len(first)==1 and len(second)==1
    return first[0]/second[0]

@impl('neg')
def impl_neg(xs):
    assert len(xs)==1
    return -xs[0][1]

@impl('max')
def impl_max(xs):
    assert len(xs)>=1
    return max(v for _,v in xs)

@impl('min')
def impl_min(xs):
    assert len(xs)>=1
    return min(v for _,v in xs)

def runtime_graph(file:str,program:Program,r:RunType)->Graph:
    decls=[t for u in program.universes for t in u.types if t.full==r.name and (not t.function or t.return_all)]
    gs=[]
    for t in decls:
        if t.return_all:
            g,ins,_=Builder(file,program).build_function(t)
            for name,gid in ins.items():
                if gid in g.nodes: g.nodes[gid].input_names.add(name)
            gs.append(g)
        else:
            gs.extend(build_variants(file,program,t))
    if len(gs)!=1:
        kinds=', '.join(('return all function' if t.return_all else 'data type') for t in decls) or '<none>'
        raise Error(file,r.span,f"run {r.name!r} requires exactly one resolved executable graph variant, but {len(gs)} are available from: {kinds}. Make the run target structurally unambiguous")
    g=gs[0]
    routes=[e for e in g.edges if not e.tag.startswith('!') and (e.tag=='returns' or e.tag.startswith('arg'))]
    indeg={i:0 for i in g.nodes}
    for e in routes: indeg[e.right]+=1
    q=[i for i,d in indeg.items() if d==0]; seen=[]; work=indeg.copy()
    while q:
        i=q.pop(); seen.append(i)
        for e in routes:
            if e.left==i:
                work[e.right]-=1
                if work[e.right]==0:q.append(e.right)
    if len(seen)!=len(g.nodes): raise Error(file,r.span,f'cannot run {r.name}: directed cycle detected; execution graphs must be acyclic',g)
    return g

def call_name(n:Node)->str|None:
    xs=[t for t in n.types if is_string_literal(t)]
    if len(xs)!=1: return None
    t=xs[0]; u,v=t.rsplit('::',1) if '::' in t else ('Impl',t); return f'{u}::{v[1:-1]}'
def call_routes(g:Graph)->tuple[dict[int,list[Edge]],dict[int,Edge]]:
    args,rets={},{}
    for e in g.edges:
        if not e.tag.startswith('!') and (e.tag=='arg' or e.tag.startswith('arg') and e.tag[3:].isdigit()): args.setdefault(e.right,[]).append(e)
        elif e.tag=='returns':
            if e.left in rets: raise ValueError(f"call {best_name(g.nodes[e.left])!r} has multiple returns routes")
            rets[e.left]=e
    return args,rets
def runtime_roots(g:Graph)->list[int]:
    args,rets=call_routes(g); calls=set(args)|set(rets); produced={e.right for e in rets.values()}; return [i for i in g.nodes if i not in calls and i not in produced]
def runtime_sinks(g:Graph)->list[int]:
    args,rets=call_routes(g); used={e.left for es in args.values() for e in es}; produced={e.right for e in rets.values()}; return [i for i in produced if i not in used] or [i for i in runtime_roots(g) if i not in used]
def node_literal_value(n:Node):
    xs=[t for t in n.types if is_literal(t)]
    if len(xs)!=1: return None
    raw=literal_text(xs[0])
    if is_string_literal(raw):
        try: return json.loads(raw)
        except Exception: return raw[1:-1]
    if is_numeric_literal(raw):
        try: return float(raw) if any(c in raw for c in '.eE') else int(raw)
        except ValueError: return None
    return None

def root_converter(n:Node):
    cs=[(t,TYPE_CONVERTERS[t]) for t in n.types if t in TYPE_CONVERTERS]
    if len(cs)!=1: raise ValueError(f"root {best_name(n)!r} needs exactly one converter; available types are {', '.join(sorted(n.types)) or 'none'}")
    return cs[0]
def execute_graph(g:Graph,raw:dict[str,str])->tuple[dict[int,object],list[int]]:
    values={}; roots=runtime_roots(g); names=graph_names(g,set(roots))
    for i in roots:
        n=g.nodes[i]; key=names[i]; literal=node_literal_value(n)
        if literal is not None:
            values[i]=literal; continue
        typ,conv=root_converter(n)
        if key not in raw: raise ValueError(f'missing input for {key} ({typ})')
        try: values[i]=conv(raw[key])
        except Exception as e: raise ValueError(f'cannot convert input {key}={raw[key]!r} as {typ}: {e}') from e
    args,rets=call_routes(g); pending=set(args)|set(rets)
    while pending:
        progress=False
        for i in list(pending):
            if i not in args or i not in rets: raise ValueError(f"call {best_name(g.nodes[i])!r} requires arg/argN routes and exactly one returns route")
            ins=args[i]
            if any(e.left not in values for e in ins): continue
            if all(e.tag=='arg' for e in ins): ordered=ins
            elif all(e.tag.startswith('arg') and e.tag[3:].isdigit() for e in ins):
                numbered=sorted((int(e.tag[3:]),e) for e in ins)
                if [n for n,_ in numbered]!=list(range(len(numbered))): raise ValueError(f"call {best_name(g.nodes[i])!r} requires contiguous argN routes starting at arg0")
                ordered=[e for _,e in numbered]
            else: raise ValueError(f"call {best_name(g.nodes[i])!r} cannot mix variadic arg routes with positional argN routes")
            fn=call_name(g.nodes[i])
            if fn is None: raise ValueError(f"call {best_name(g.nodes[i])!r} needs exactly one string literal implementation type")
            impl=IMPL_FUNCTIONS.get(fn)
            if impl is None: raise ValueError(f'no Python implementation registered for {fn}')
            values[rets[i].right]=impl([(e.tag,values[e.left]) for e in ordered]); pending.remove(i); progress=True
        if not progress: raise ValueError('execution stalled on unresolved call dependencies')
    return values,runtime_sinks(g)
def console_runs(file:str,program:Program)->None:
    for r in program.runs:
        g=runtime_graph(file,program,r); raw={}; roots=runtime_roots(g); names=graph_names(g,set(roots))
        print(f'\n{kw("run")} {typefmt(r.name)}')
        for i in roots:
            n=g.nodes[i]
            if node_literal_value(n) is not None: continue
            typ,_=root_converter(n); raw[names[i]]=input(f'{names[i]} ({typ}): ')
        values,sinks=execute_graph(g,raw)
        for i in sinks: print(f'{cyan(best_name(g.nodes[i]))} {gray("=")} {values[i]}')

def print_program(program:Program,builder:Builder)->None:
    for ui,u in enumerate(program.universes):
        if ui: print()
        print(f'{C.BOLD}{kw("universe")} {green(u.name)}{C.RESET}')
        for t in [x for x in u.types if not is_literal(x.name)]:
            print(); print(f'{kw("def")} {green(t.name)}')
            if t.function:
                g,ins,outs=builder.build_function(t); ig,iins=builder.function_input(t); print(kw('input')); print_subgraph(ig,set(iins.values()),4); print(kw('return')); print_subgraph(g,set(outs.values()),4)
            else:
                vs=build_variants(builder.file,program,t)
                for vi,g in enumerate(vs):
                    if len(vs)>1: print(gray(f'[variant {vi+1}/{len(vs)}]'))
                    print_graph(g)

def h(s:str)->str: return html.escape(s)
def html_type(s:str)->str:
    if '::' not in s: return f'<span class="literal">{h(s)}</span>' if is_literal(s) else f'<span class="type">{h(s)}</span>'
    u,n=s.rsplit('::',1); cls='literal' if is_literal(n) else 'type'; return f'<span class="qualifier">{h(u)}::</span><span class="{cls}">{h(n)}</span>'
def html_name(s:str)->str: return f'<span class="name">{h(s)}</span>'
def html_node(n:Node,name:str)->str: return f'<div class="statement">{html_name(name)} <span class="muted">:</span> '+ '<span class="muted"> | </span>'.join(html_type(x) for x in sorted(n.types)) + '</div>'
def html_edge(g:Graph,e:Edge,names:dict[int,str])->str: return f'<div class="statement">{html_name(names[e.left])} <span class="relation">{h(e.tag)}</span> {html_name(names[e.right])}</div>'
def html_graph(g:Graph,ids:set[int]|None=None)->str:
    ids=set(g.nodes) if ids is None else set(ids)
    if not ids: return '<div class="empty">∅</div>'
    names=graph_names(g,ids)
    return '\n'.join([html_node(g.nodes[i],names[i]) for i in sorted(ids,key=lambda i:names[i])]+[html_edge(g,e,names) for e in g.edges if e.left in ids and e.right in ids])

def graph_data(g:Graph,ids:set[int]|None=None)->str:
    ids=set(g.nodes) if ids is None else set(ids); names=graph_names(g,ids)
    data={"nodes":[{"key":str(i),"id":i,"label":names[i]+' : '+' | '.join(sorted(g.nodes[i].types)),"types":sorted(g.nodes[i].types),"aliases":([names[i]]+[x for x in aliases(g.nodes[i]) if x!=names[i]])} for i in ids],"edges":[{"key":str(k),"source":str(e.left),"target":str(e.right),"label":e.tag} for k,e in enumerate(g.edges) if e.left in ids and e.right in ids]}
    return h(json.dumps(data))
def graph_button(g:Graph,ids:set[int]|None=None,label:str='👁')->str: return f'<button class="graph-btn" title="View graph" aria-label="View graph" data-graph="{graph_data(g,ids)}">{h(label)}</button>'
def error_card(e:Error)->str:
    p=e.span.start; snippet=''
    lines=source_lines(e.file)
    if 1<=p.line<=len(lines):
        line=lines[p.line-1]; a=max(0,p.column-1); n=max(1,e.span.end.column-p.column) if e.span.end.line==p.line else 1; n=min(max(1,n),max(1,len(line)-a)); snippet=f'<pre class="error-source"><span>{p.line:>4} | </span>{h(line)}\n<span>     | </span>{" "*a}<b>{"^"*n}</b></pre>'
    context=f'<pre class="error-source">{h(graph_diagnostic(e.graph))}</pre>' if e.graph is not None else ''
    return f'<div class="error-card"><strong>error</strong><span>{h(e.message)}</span><small>{h(e.file)}:{p.line}:{p.column}</small>{snippet}{context}{graph_button(e.graph) if e.graph is not None else ""}</div>' 
def source_html(source:str,error:Error|None)->str:
    lines=source.splitlines() or ['']; out=[]
    for no,line in enumerate(lines,1):
        text=h(line)
        if error and no==error.span.start.line:
            a=max(0,error.span.start.column-1); b=max(a+1,error.span.end.column-1) if error.span.end.line==no else a+1; raw=line; text=h(raw[:a])+f'<span class="source-error">{h(raw[a:b] or " ")}</span>'+h(raw[b:])
        cls=' source-error-line' if error and no==error.span.start.line else ''
        out.append(f'<div class="source-line{cls}" id="L{no}"><span class="ln">{no}</span><span class="src">{text}</span></div>')
        if error and no==error.span.start.line: out.append(error_card(error))
    return '\n'.join(out)
def run_html(program:Program,file:str)->str:
    out=[]
    for ri,r in enumerate(program.runs):
        try:
            g=runtime_graph(file,program,r); roots=runtime_roots(g); fields=[]; names=graph_names(g,set(roots))
            for i in roots:
                n=g.nodes[i]
                if node_literal_value(n) is not None: continue
                typ,_=root_converter(n); fields.append(f'<label class="run-field"><span>{h(names[i])} <small>{h(typ)}</small></span><input data-name="{h(names[i])}"></label>')
            out.append(f'<article class="run-card" data-run="{ri}"><h3><span class="keyword">run</span> {html_type(r.name)}{graph_button(g)}</h3><div class="run-inputs">{"".join(fields)}</div><button class="run-exec">Run</button><div class="run-results"></div></article>')
        except (Error,ValueError) as e:
            out.append(error_card(e) if isinstance(e,Error) else f'<div class="error-card"><strong>run error</strong><span>{h(str(e))}</span></div>')
    return ''.join(out)

def inferred_html(program:Program,file:str,limit:Error|None)->str:
    builder=Builder(file,program); body=[]; stopped=False
    for u in program.universes:
        ub=[]; failed=False
        for t in [x for x in u.types if not is_literal(x.name)]:
            ub.append(f'<article><h3><span class="keyword">def</span> <span class="type">{h(t.name)}</span></h3>')
            try:
                if t.function:
                    g,ins,outs=builder.build_function(t); ig,iins=builder.function_input(t); ub.append('<div class="section-keyword">inputs '+graph_button(ig,set(iins.values()))+'</div><div class="function-body">'+html_graph(ig,set(iins.values()))+'</div><div class="section-keyword">return type '+graph_button(g,set(outs.values()))+'</div><div class="function-body">'+html_graph(g,set(outs.values()))+'</div>')
                else:
                    vs=build_variants(file,program,t)
                    for vi,g in enumerate(vs):
                        if len(vs)>1: ub.append(f'<div class="muted">variant {vi+1}/{len(vs)}</div>')
                        ub.append(graph_button(g)); ub.append('<div>'+html_graph(g)+'</div>')
            except Error as e:
                ub.append(error_card(e)); failed=True
            ub.append('</article>')
        body.append(f'<details class="universe"{" open" if failed or limit else ""}><summary><span class="keyword">universe</span> <span class="type">{h(u.name)}</span></summary>'+''.join(ub)+'</details>')
    body.append(run_html(program,file))
    return '\n'.join(body)
def html_document(program:Program,file:str,source:str,error:Error|None)->str:
    title=h(Path(file).name); inferred=inferred_html(program,file,error); src=source_html(source,error); jump=f'<script>document.getElementById("L{error.span.start.line}")?.scrollIntoView({{block:"center"}})</script>' if error else ''
    return f'''<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>{title}</title><style>
:root{{--bg:#101218;--panel:#171a22;--panel2:#12151c;--border:#2a3040;--text:#d9deea;--muted:#747d91;--purple:#c792ea;--green:#8bd49c;--yellow:#ffd580;--cyan:#89ddff;--red:#ff6b78}}*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--text);font:14px/1.65 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}}header{{height:58px;display:flex;align-items:center;padding:0 24px;border-bottom:1px solid var(--border);position:sticky;top:0;background:#101218ee;backdrop-filter:blur(12px);z-index:2}}header b{{font-size:16px}}header span{{margin-left:auto;color:var(--muted)}}main{{display:grid;grid-template-columns:minmax(360px,1fr) minmax(420px,1fr);min-height:calc(100vh - 58px)}}.source,.inference{{padding:28px 24px 80px;overflow:auto}}.source{{background:var(--panel2);border-right:1px solid var(--border)}}.pane-title{{color:var(--muted);text-transform:uppercase;letter-spacing:.12em;font-size:11px;margin-bottom:18px}}.source-line{{display:grid;grid-template-columns:48px 1fr;white-space:pre;min-height:1.65em}}.ln{{color:#4f586c;text-align:right;padding-right:16px;user-select:none}}.src{{white-space:pre}}.source-error-line{{background:#ff6b780d}}.source-error{{color:#fff;text-decoration:underline;text-decoration-color:var(--red);text-decoration-thickness:2px;text-underline-offset:3px;background:#ff6b7820}}section{{margin-bottom:34px}}h2,h3{{font:inherit;margin:0 0 12px}}h2{{font-weight:700;font-size:17px}}h3{{font-weight:600;font-size:15px}}article{{margin:18px 0;padding:18px 20px;background:var(--panel);border:1px solid var(--border);border-radius:10px}}.statement{{white-space:pre-wrap;min-height:1.65em}}.function-body{{margin:3px 0 13px 22px}}.section-keyword,.keyword{{color:var(--purple);font-weight:600}}.type{{color:var(--green)}}.literal{{color:var(--yellow)}}.qualifier,.muted,.bracket,.equal,.empty{{color:var(--muted)}}.name{{color:var(--cyan)}}.relation{{color:#f07178;font-weight:700}}.error-card{{display:grid;gap:4px;margin:12px 0;padding:12px 14px;border:1px solid #ff6b7860;border-left:4px solid var(--red);border-radius:7px;background:#ff6b7810;color:#ffc2c7}}.error-card strong{{color:var(--red)}}.error-card>span{{white-space:pre-wrap}}.error-card small{{color:#a66f78}}.error-source{{margin:6px 0 0;padding:8px 10px;overflow:auto;background:#0d1017;border-radius:5px;color:#d9deea;white-space:pre}}.error-source span{{color:#747d91}}.error-source b{{color:#ff6b78;font-weight:700}}.graph-modal{{display:none;position:fixed;inset:0;z-index:100;background:#080a0fcc;align-items:center;justify-content:center;padding:28px}}.graph-modal.open{{display:flex}}.graph-box{{position:relative;width:min(1000px,92vw);height:min(820px,88vh);background:var(--panel);border:1px solid var(--border);border-radius:12px;padding:62px 16px 16px;overflow:hidden}}.sigma-wrap{{position:relative;width:100%;height:min(75vh,740px);min-height:360px;overflow:hidden;border-radius:8px}}.sigma-host{{position:absolute;inset:0;z-index:1;overflow:hidden}}.edge-overlay{{position:absolute;inset:0;width:100%;height:100%;pointer-events:none;z-index:2}}.node-label-overlay{{position:absolute;inset:0;pointer-events:none;z-index:3}}.node-label{{position:absolute;transform:translate(-50%,-100%);margin-top:-14px;color:#f8fafc;font-weight:700;font-size:13px;text-shadow:0 1px 3px #000,0 0 5px #000;white-space:nowrap}}.graph-tip{{display:none;position:absolute;z-index:5;max-width:min(440px,70%);padding:10px 12px;background:#0b0e14f2;border:1px solid #465067;border-radius:8px;color:#f3f6fc;box-shadow:0 8px 28px #0008;pointer-events:none;white-space:pre-wrap;font-size:12px;line-height:1.5}}.graph-tip.open{{display:block}}.graph-close{{position:absolute;right:14px;top:12px;z-index:6}}.graph-hint{{position:absolute;left:18px;top:17px;z-index:6;color:var(--muted);font-size:11px;pointer-events:none}}.graph-btn{{float:right;border:0;background:transparent;color:inherit;font-size:17px;line-height:1;cursor:pointer;padding:3px 5px;border-radius:5px}}.graph-btn:hover{{background:#ffffff12}}details.universe{{margin-bottom:18px}}details.universe>summary{{cursor:pointer;font-weight:700;font-size:17px;margin-bottom:12px}}details.universe:not([open])>summary{{margin-bottom:0}}.run-inputs{{display:grid;gap:8px;margin:12px 0}}.run-field{{display:grid;grid-template-columns:max-content minmax(0,1fr);align-items:center;gap:12px}}.run-field input{{width:100%;min-width:0}}.run-exec{{display:block;width:100%;margin:12px 0;background:var(--green);border:1px solid var(--green);color:var(--bg);border-radius:7px;padding:7px 15px;font:inherit;font-weight:700;cursor:pointer}}@media(max-width:850px){{main{{grid-template-columns:1fr}}.source{{border-right:0;border-bottom:1px solid var(--border)}}}}</style></head><body><header><b>{title}</b><span>{'stopped at error' if error else 'inference complete'}</span></header><main><div class="source"><div class="pane-title">source</div>{src}</div><div class="inference"><div class="pane-title">inference</div>{inferred}</div></main><div id="graphModal" class="graph-modal"><div class="graph-box"><button class="graph-close">Close</button><div class="graph-hint">Scroll/pinch to zoom · drag to pan</div><div class="sigma-wrap"><div id="sigmaHost" class="sigma-host"></div><canvas id="edgeOverlay" class="edge-overlay"></canvas><div id="nodeLabelOverlay" class="node-label-overlay"></div><div id="graphTip" class="graph-tip"></div></div></div></div><script type="module">import Graph from 'https://cdn.jsdelivr.net/npm/graphology@0.26.0/+esm';import Sigma from 'https://cdn.jsdelivr.net/npm/sigma@3.0.2/+esm';let renderer=null,currentData=null;const modal=document.getElementById('graphModal'),host=document.getElementById('sigmaHost'),overlay=document.getElementById('edgeOverlay'),labels=document.getElementById('nodeLabelOverlay'),tip=document.getElementById('graphTip');function drawLabels(){{if(!renderer||!currentData)return;labels.replaceChildren();for(const n of currentData.nodes){{const p=renderer.graphToViewport(renderer.getGraph().getNodeAttributes(n.key)),el=document.createElement('div');el.className='node-label';el.textContent=n.label;el.style.left=p.x+'px';el.style.top=p.y+'px';labels.appendChild(el)}}}}function drawEdges(){{if(!renderer||!currentData)return;const dpr=devicePixelRatio||1,r=overlay.getBoundingClientRect();overlay.width=Math.max(1,Math.round(r.width*dpr));overlay.height=Math.max(1,Math.round(r.height*dpr));const c=overlay.getContext('2d');c.scale(dpr,dpr);c.clearRect(0,0,r.width,r.height);c.font='12px ui-monospace,SFMono-Regular,Menlo,Consolas,monospace';c.textAlign='center';c.textBaseline='bottom';for(const e of currentData.edges){{const sa=renderer.getGraph().getNodeAttributes(e.source),ta=renderer.getGraph().getNodeAttributes(e.target),a=renderer.graphToViewport(sa),b=renderer.graphToViewport(ta),dx=b.x-a.x,dy=b.y-a.y,len=Math.hypot(dx,dy)||1,ux=dx/len,uy=dy/len,ex=b.x-ux*12,ey=b.y-uy*12;c.beginPath();c.setLineDash([7,6]);c.lineWidth=1.5;c.strokeStyle='#ff6b78';c.moveTo(a.x,a.y);c.lineTo(ex,ey);c.stroke();c.setLineDash([]);c.beginPath();c.fillStyle='#ff6b78';c.moveTo(ex,ey);c.lineTo(ex-ux*9-uy*5,ey-uy*9+ux*5);c.lineTo(ex-ux*9+uy*5,ey-uy*9-ux*5);c.closePath();c.fill();const mx=(a.x+ex)/2,my=(a.y+ey)/2;c.lineWidth=4;c.strokeStyle='#11151d';c.strokeText(e.label,mx,my-3);c.fillStyle='#ff6b78';c.fillText(e.label,mx,my-3)}}drawLabels()}}function nodeInfo(key){{const n=currentData.nodes.find(x=>x.key===key);let names=Object.entries(n.names||{{}}).map(([u,ns])=>[u,(ns||[]).filter(x=>x&&x.trim())]).filter(([,ns])=>ns.length).map(([u,ns])=>u+':: '+ns.join(' = ')).join('\\n');return 'types: '+(n.types.join(' | ')||'∅')+(names?'\\naliases:\\n'+names:'')}}function showTip(e){{tip.textContent=nodeInfo(e.node);tip.classList.add('open');const p=renderer.graphToViewport(renderer.getGraph().getNodeAttributes(e.node)),r=tip.parentElement.getBoundingClientRect();tip.style.left=Math.min(r.width-tip.offsetWidth-8,p.x+16)+'px';tip.style.top=Math.min(r.height-tip.offsetHeight-8,p.y+16)+'px'}}window.openSigma=(data)=>{{if(renderer){{renderer.kill();renderer=null}}host.replaceChildren();tip.classList.remove('open');currentData=data;modal.classList.add('open');requestAnimationFrame(()=>requestAnimationFrame(()=>{{const r=host.getBoundingClientRect();if(!r.width||!r.height){{console.error('Graph viewport has no layout size',r);return}}const g=new Graph(),n=data.nodes.length;data.nodes.forEach((x,i)=>g.addNode(x.key,{{label:'',x:Math.cos(i*2*Math.PI/Math.max(1,n)),y:Math.sin(i*2*Math.PI/Math.max(1,n)),size:13,color:'#334155',forceLabel:false}}));data.edges.forEach(e=>g.addEdgeWithKey(e.key,e.source,e.target,{{label:e.label,size:.1,color:'#00000000'}}));renderer=new Sigma(g,host,{{renderEdgeLabels:false,labelColor:{{color:'#f8fafc'}},labelSize:13,labelWeight:'600',defaultEdgeType:'line',minCameraRatio:.25,maxCameraRatio:4}});renderer.on('afterRender',drawEdges);renderer.on('enterNode',showTip);renderer.on('leaveNode',()=>tip.classList.remove('open'));drawEdges()}}))}};document.addEventListener('click',e=>{{const b=e.target.closest('.graph-btn');if(b)openSigma(JSON.parse(b.dataset.graph));if(e.target.closest('.graph-close')||e.target===modal){{modal.classList.remove('open');tip.classList.remove('open')}}}});</script>{jump}</body></html>'''
def export_html(program:Program,file:str,source:str,error:Error|None)->Path:
    path=Path(file).with_suffix('.html').resolve(); path.write_text(html_document(program,file,source,error),encoding='utf-8'); webbrowser.open(path.as_uri()); return path

def playground_document(initial:str,title:str)->str:
    base=html_document(Program(),title,initial,None)
    css="""<style>#play{position:fixed;inset:58px 50% 0 0;z-index:5;background:#12151c;border-right:1px solid #2a3040}.editwrap{position:absolute;inset:38px 0 0}.editwrap textarea,.editwrap pre{position:absolute;inset:0;margin:0;padding:20px 22px;border:0;overflow:auto;font:14px/1.65 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;tab-size:4;white-space:pre}.editwrap textarea{z-index:2;resize:none;background:transparent;color:transparent;caret-color:white;-webkit-text-fill-color:transparent;outline:none}.editwrap pre{z-index:1;pointer-events:none;color:#d9deea}.hl-k{color:#c792ea;font-weight:600}.hl-t{color:#8bd49c}.hl-s{color:#ffd580}.hl-c{color:#596174}.hl-o{color:#f07178}#rerun{margin-left:14px;color:#d9deea;background:#222735;border:1px solid #384055;border-radius:7px;padding:6px 15px;font:inherit;cursor:pointer}main{grid-template-columns:1fr}main .source{display:none}main .inference{margin-left:50%;width:50%}@media(max-width:850px){#play{position:relative;inset:auto;height:50vh}main .inference{margin-left:0;width:100%}}</style>"""
    script=r"""<script>const ed=document.getElementById('editor'),hc=document.getElementById('highlightCode'),hp=document.getElementById('highlight'),btn=document.getElementById('rerun'),inf=document.querySelector('.inference');function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}function scan(s){let types=new Set(),rels=new Set(),ts=[],i=0;while(i<s.length){if(s[i]=='\n'||s[i]==','){ts.push(['sep',s[i]]);i++;continue}if(/\s/.test(s[i])){i++;continue}if(s[i]=='/'&&s[i+1]=='/'){let j=s.indexOf('\n',i);i=j<0?s.length:j;continue}if(s[i]=='"'){let j=i+1;while(j<s.length){if(s[j]=='\\')j+=2;else if(s[j]=='"'){j++;break}else j++}ts.push(['str',s.slice(i,j)]);i=j;continue}let m=s.slice(i).match(/^[A-Za-z_][A-Za-z0-9_.]*(?:::[A-Za-z_][A-Za-z0-9_.]*)*/);if(m){let v=m[0],k=/^(universe|namespace|def|uses|return|where|reduce|all|do|run|import)$/.test(v)?'kw':'name';ts.push([k,v]);i+=v.length;continue}m=s.slice(i).match(/^[~=<>!+\-*\/%^&@#$?\\]+/);if(m){ts.push(['rel',m[0]]);i+=m[0].length;continue}if(s[i]==':'||s[i]=='|'||s[i]=='('||s[i]==')'){ts.push([s[i],s[i]]);i++;continue}i++}for(let j=0;j+1<ts.length;j++)if(ts[j][0]=='kw'&&/^(type|namespace|universe)$/.test(ts[j][1])&&(ts[j+1][0]=='name'||ts[j+1][0]=='str'))types.add(ts[j+1][1]);let j=0,mode='top';while(j<ts.length){let t=ts[j];if(t[0]=='kw'){if(t[1]=='universe'||t[1]=='namespace'||t[1]=='type'){j+=2;mode=t[1]=='type'?'body':'top';continue}if(t[1]=='return'||t[1]=='where'){mode=t[1];j++;continue}if(t[1]=='run'||t[1]=='import'){mode='top';j+=2;continue}j++;continue}if(mode=='top'||t[0]!='name'){j++;continue}if(j+1<ts.length&&ts[j+1][0]==':'){j+=2;if(j<ts.length&&(ts[j][0]=='name'||ts[j][0]=='str'))j++;while(j+1<ts.length&&ts[j][0]=='|'&&(ts[j+1][0]=='name'||ts[j+1][0]=='str'))j+=2;continue}if(j+2<ts.length&&(ts[j+1][0]=='rel'||ts[j+1][0]=='name')&&(ts[j+2][0]=='name'||ts[j+2][0]=='str'||ts[j+2][0]=='kw')){let mid=ts[j+1],right=ts[j+2];if(!(mid[1]=='='&&right[0]=='kw'&&right[1]=='reduce'))rels.add(mid[1]);j+=3;continue}j++}return{types,rels}}function hi(){let s=ed.value,o='',i=0,{types,rels}=scan(s);while(i<s.length){if(s[i]=='('||s[i]==')'){o+='<span class="hl-s">'+s[i]+'</span>';i++;continue}if(s[i]=='/'&&s[i+1]=='/'){let j=s.indexOf('\n',i);if(j<0)j=s.length;o+='<span class="hl-c">'+esc(s.slice(i,j))+'</span>';i=j;continue}if(s[i]=='"'){let j=i+1;while(j<s.length){if(s[j]=='\\')j+=2;else if(s[j]=='"'){j++;break}else j++}o+='<span class="hl-s">'+esc(s.slice(i,j))+'</span>';i=j;continue}let m=s.slice(i).match(/^(universe|namespace|def|uses|return|where|reduce|all|do|run|import)\b/);if(m){o+='<span class="hl-k">'+m[0]+'</span>';i+=m[0].length;continue}m=s.slice(i).match(/^[A-Za-z_][A-Za-z0-9_]*/);if(m&&(types.has(m[0])||rels.has(m[0]))){o+='<span class="'+(types.has(m[0])?'hl-t':'hl-o')+'">'+esc(m[0])+'</span>';i+=m[0].length;continue}m=s.slice(i).match(/^[~=<>!+\-*\/%^&@#$?\\]+/);if(m&&rels.has(m[0])){o+='<span class="hl-o">'+esc(m[0])+'</span>';i+=m[0].length;continue}o+=esc(s[i++])}hc.innerHTML=o+'\n'}async function go(){btn.disabled=true;btn.textContent='Running…';try{let r=await fetch('/run',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({source:ed.value})}),d=await r.json();inf.innerHTML='<div class="pane-title">inference</div>'+d.html;requestAnimationFrame(()=>{let e=inf.querySelector('.error-card');if(e)e.scrollIntoView({block:'end',behavior:'smooth'});else inf.scrollTop=inf.scrollHeight})}catch(e){inf.innerHTML='<div class="error-card"><strong>server error</strong><span>'+esc(String(e))+'</span></div>';requestAnimationFrame(()=>inf.querySelector('.error-card')?.scrollIntoView({block:'end',behavior:'smooth'}))}finally{btn.disabled=false;btn.textContent='Run'}}let timer;ed.addEventListener('input',()=>{hi();clearTimeout(timer);timer=setTimeout(go,300)});ed.addEventListener('scroll',()=>{hp.scrollTop=ed.scrollTop;hp.scrollLeft=ed.scrollLeft});ed.addEventListener('keydown',e=>{if(e.key==='Tab'){e.preventDefault();let a=ed.selectionStart,b=ed.selectionEnd;ed.setRangeText('    ',a,b,'end');hi()}if((e.ctrlKey||e.metaKey)&&e.key==='Enter'){e.preventDefault();go()}});document.addEventListener('click',async e=>{let b=e.target.closest('.run-exec');if(!b)return;let card=b.closest('.run-card'),values={};card.querySelectorAll('input[data-name]').forEach(x=>values[x.dataset.name]=x.value);b.disabled=true;try{let r=await fetch('/execute',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({source:ed.value,run:Number(card.dataset.run),values})}),d=await r.json(),out=card.querySelector('.run-results');if(d.error)out.innerHTML='<div class="error-card"><strong>run error</strong><span>'+esc(d.error)+'</span></div>';else out.innerHTML=d.results.map(x=>'<div class="statement"><span class="name">'+esc(x.name)+'</span> <span class="equal">=</span> '+esc(x.value)+'</div>').join('');requestAnimationFrame(()=>requestAnimationFrame(()=>out.scrollIntoView({block:'end',behavior:'smooth'})))}finally{b.disabled=false}});btn.onclick=go;hi();if(ed.value.trim())go();</script>"""
    editor=f'<div id="play"><div class="pane-title">source</div><div class="editwrap"><pre id="highlight"><code id="highlightCode"></code></pre><textarea id="editor" spellcheck="false">{h(initial)}</textarea></div></div>'
    base=base.replace('</head>',css+'</head>').replace('</header>', '<button id="rerun">Run</button></header>').replace('<main>',editor+'<main>').replace('</body>',script+'</body>')
    return base

def infer_editor(source:str,file:str='<editor>')->tuple[str,Error|None]:
    program=Program(); parser=None
    try:
        program=load_program(file,source); Resolver(file,program).resolve(); return inferred_html(program,file,None),None
    except Error as e:
        program=getattr(e,'program',program)
        if parser is not None: program=parser.partial_program()
        try: Resolver(file,program).resolve()
        except Error: pass
        rendered=inferred_html(program,file,e); return rendered if 'error-card' in rendered else rendered+error_card(e),e

def serve(initial:str='',title:str='GraS',file:str='<editor>')->None:
    page=playground_document(initial,title).encode('utf-8')
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,fmt,*args)->None: return
        def reply(self,status:int,kind:str,data:bytes)->None: self.send_response(status); self.send_header('Content-Type',kind); self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
        def do_GET(self)->None:
            if self.path!='/': self.reply(404,'text/plain; charset=utf-8',b'not found'); return
            self.reply(200,'text/html; charset=utf-8',page)
        def do_POST(self)->None:
            if self.path not in ('/run','/execute'): self.reply(404,'text/plain; charset=utf-8',b'not found'); return
            try:
                n=int(self.headers.get('Content-Length','0')); payload=json.loads(self.rfile.read(n) or b'{}'); source=str(payload.get('source',''))
                if self.path=='/run':
                    rendered,error=infer_editor(source,file); data={'html':rendered,'error':error is not None}
                else:
                    program=load_program(file,source); Resolver(file,program).resolve(); ri=int(payload.get('run',0))
                    if ri<0 or ri>=len(program.runs): raise ValueError('unknown run request')
                    g=runtime_graph(file,program,program.runs[ri]); values,sinks=execute_graph(g,{str(k):str(v) for k,v in dict(payload.get('values',{})).items()}); data={'results':[{'name':best_name(g.nodes[i]),'value':str(values[i])} for i in sinks]}
                self.reply(200,'application/json; charset=utf-8',json.dumps(data).encode())
            except Exception as e: self.reply(200,'application/json; charset=utf-8',json.dumps({'error':str(e)}).encode())
    server=ThreadingHTTPServer(('127.0.0.1',0),Handler); url=f'http://127.0.0.1:{server.server_port}/'; print(f'{green("serving")} {url}'); threading.Timer(.15,lambda:webbrowser.open(url)).start()
    try: server.serve_forever()
    except KeyboardInterrupt: print(); print(gray('server stopped'))
    finally: server.server_close()

def main()->None:
    args=sys.argv[1:]; serve_mode='--serve' in args; args=[x for x in args if x!='--serve']
    if len(args)>1: print(f'usage: {sys.argv[0]} [--serve] [FILE]'); raise SystemExit(2)
    if not args: serve(); return
    file=args[0]; source=Path(file).read_text(encoding='utf-8')
    if serve_mode: serve(source,Path(file).name,str(Path(file).resolve())); return
    try:
        program=load_program(file,source); Resolver(file,program).resolve(); print_program(program,Builder(file,program)); console_runs(file,program)
    except Error as e: print(e.pretty(),file=sys.stderr); raise SystemExit(1)

if __name__=='__main__': main()
