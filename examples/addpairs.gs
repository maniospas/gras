import gs.impl
uses Impl

def addpairs(x:nat,y:nat,z:nat,x followedby y, y followedby z)
return all r1 followedby r2 where
    r1 = add(x,y)
    r2 = add(y,z)

run addpairs
