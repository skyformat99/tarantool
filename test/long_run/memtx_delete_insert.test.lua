
dofile('suite.lua')

engine_name = 'memtx'

math.randomseed(1)
delete_insert(engine_name)