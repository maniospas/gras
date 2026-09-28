import gs.impl
uses Impl

def pair
    x:nat
    y:nat
return
    x:nat
    y:nat
    x followedby y

def main
    p1: pair
return all where
    r3 = Impl::add(p1) // the difficulty is that the 'followedby' edge is different

run main