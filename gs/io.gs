import gs.impl
import gs.array

universe IO
uses Impl
uses Array
def sarray

//  String / numeric conversions

def tonat(a:string)
return (
    call:"ston",
    a:string,
    result:nat,
    call returns result,
    a arg call
)

def tofloat(a:string)
return (
    call:"stof",
    a:string,
    result:float,
    call returns result,
    a arg call
)

def tostring(a:nat)
return (
    call:"ntos",
    a:nat,
    result:string,
    call returns result,
    a arg call
)

def tostring(a:float)
return (
    call:"ftos",
    a:float,
    result:string,
    call returns result,
    a arg call
)


//  String operations

def cat(a:string,b:string,a followedby b)
return (
    call:"scat",
    a:string,
    b:string,
    result:string,
    call returns result,
    a arg0 call,
    b arg1 call
)

def get(a:string,i:nat)
return (
    call:"nget",
    a:string,
    i:nat,
    result:nat,
    call returns result,
    a arg0 call,
    i arg1 call
)

def slice(a:string,start:nat,size:nat)
return (
    call:"sslice",
    a:string,
    start:nat,
    size:nat,
    result:string,
    call returns result,
    a arg0 call,
    start arg1 call,
    size arg2 call
)

def split(a:string,separator:string,a followedby separator)
return (
    call:"ssplit",
    a:string,
    separator:string,
    result:sarray,
    call returns result,
    a arg0 call,
    separator arg1 call
)


//  File loading

def load(path:string)
return (
    call:"loadstring",
    path:string,
    result:string,
    call returns result,
    path arg call
)

def loadlines(path:string)
return (
    call:"loadsarray",
    path:string,
    result:sarray,
    call returns result,
    path arg call
)


//  File saving

def save(path:string,text:string,path followedby text)
return (
    call:"savestring",
    path:string,
    text:string,
    result:nat,
    call returns result,
    path arg0 call,
    text arg1 call
)

def savelines(path:string,lines:sarray,path followedby lines)
return (
    call:"savesarray",
    path:string,
    lines:sarray,
    result:nat,
    call returns result,
    path arg0 call,
    lines arg1 call
)