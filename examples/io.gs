import gs.io
uses Impl
uses Array
uses IO

def main(path:string, converter:string) // try "stof" converter
return all where
    lines = loadlines(path)
    values = fmap(converter, lines)
    reduce cat(converter followedby "")
    reduce get(values,0)
    
run main