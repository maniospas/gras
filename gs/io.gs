import gs.impl
import gs.array

universe IO
uses Impl
uses Array

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