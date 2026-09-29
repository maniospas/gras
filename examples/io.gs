import gs.io

universe Global
uses Impl
uses Array
uses IO

def map(fn:string, lines:sarray)
return (
    call:"map",
    fn:string,
    lines:sarray,
    result:farray,
    call returns result,
    fn arg0 call,
    lines arg1 call
)

def main(path:string, converter:string) // try "stof" converter
return all where
    lines = loadlines(path)
    values = map(converter, lines)
    reduce cat(converter followedby "")
    reduce get(values,0)
    

run main