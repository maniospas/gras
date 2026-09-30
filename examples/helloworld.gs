// examples/helloworld.gs
import gs.impl
uses Impl

def main()
return all where
    reduce cat("hello" followedby " world!")

run main