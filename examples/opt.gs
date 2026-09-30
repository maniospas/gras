import gs.impl
import gs.field
uses Impl

def nat2num x:nat 
return x:Field::num

def main(x:nat, y:nat, z:nat)
return all where goal Impl
    reduce sub(add(add(x,y),z) followedby y)
    reduce all nat2num
    reduce all Field::merge_add
    reduce all Field::optimization_addsub

run main
