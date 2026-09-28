import gs.impl
import gs.implopt

universe Global 
uses Impl 
uses Implopt

def main(x:nat, y:nat)
return all where
    r = add(x,y)
    reduce sub(r,y,r followedby y)
    reduce all merge_add
    reduce all optimization_addsub

run main
