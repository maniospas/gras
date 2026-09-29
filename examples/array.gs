import gs.array

uses Impl
uses Array

def main()
return all where
    box_a = toarr(0.2)
    box_b = toarr(1.1)
    c = add(box_a,box_b)
    ret = add(get(c,0),2.2)
   
run main