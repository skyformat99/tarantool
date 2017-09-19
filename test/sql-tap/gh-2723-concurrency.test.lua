#!/usr/bin/env tarantool
test = require("sqltester")
test:plan(4)
local fiber = require("fiber")
local N = 20

-- this test uses ddl which is not working concurrently
-- see issue #2741
ch = fiber.channel(N)
for id = 1, N do
    fiber.create(
        function ()
            local table_name = "table2723"..id
            box.sql.execute("create table "..table_name.."(id primary key, a integer unique, b)")
            box.sql.execute("insert into "..table_name.." values(1, 2, 3)")
            box.sql.execute("insert into "..table_name.." values(3, 4, 3)")
            pcall( function() box.sql.execute("insert into "..table_name.." values(3, 4, 3)") end)
            box.sql.execute("drop table "..table_name)
            ch:put(1)
        end
    )
end
for id = 1, N do
    ch:get()
end

test:do_test(
    "concurrency:1",
    function()
        return test:execsql("select count(*) from _space where name like 'table-2723-%'")[1]
    end,
    0)

ch = fiber.channel(N)
box.sql.execute("create table t1(id primary key, a integer unique, b);")
box.sql.execute("create index i1 on t1(b);")
for id = 1, N do
    fiber.create(
        function ()
            box.sql.execute(string.format("insert into t1 values(%s, %s, 3)", id, id))
            box.sql.execute(string.format("insert into t1 values(%s, %s, 3)", id+N, id+N))
            box.sql.execute(string.format("delete from t1 where id = %s", id+N))
            box.sql.execute(string.format("insert into t1 values(%s, %s, 3)", id+2*N, id+2*N))
            box.sql.execute(string.format("delete from t1 where id = %s", id+2*N))
            ch:put(1)
        end
    )
end
for id = 1, N do
    ch:get()
end
test:do_test(
    "concurrency:2",
    function()
        return test:execsql("select count(*) from (select distinct * from t1);")[1]
    end,
    N)
box.sql.execute("drop table t1;")


ch = fiber.channel(N)
box.sql.execute("create table t1(id primary key, a integer unique, b);")
box.sql.execute("create index i1 on t1(b);")
for id = 1, N*N do
    box.sql.execute(string.format("insert into t1 values(%s, %s, 3)", id, id))
end
for id = 1, N do
    fiber.create(
        function ()
            box.sql.execute("delete from t1")
            ch:put(1)
        end
    )
end
for id = 1, N do
    ch:get()
end
test:do_test(
    "concurrency:3",
    function()
        return test:execsql("select count(*) from t1;")[1]
    end,
    0)
box.sql.execute("drop table t1;")

-------- run ordinary tests from this folder in parallel
-- 18 is the maximum number which not leads to assetrs
NUMBER_OF_TESTS = 50
script_path = arg[0]:match("(.*/)")
script_name = arg[0]:match("([^/]*)$")
function get_test_file_names()
    local i, t, popen = 0, {}, io.popen
    local pfile = popen('ls "'..script_path..'"')
    for filename in pfile:lines() do
        if i > NUMBER_OF_TESTS then break end
        if filename ~= script_name then
            i = i + 1
            table.insert(t, filename)
        end
    end
    return t
end

function blank()
end

finish_test = test.finish_test
test.finish_test = blank
plan = test.plan
test.plan = blank
fail = test.fail
test.fail = blank
ok = test.ok
test.ok = blank
io_write = test.io_write
test.io_write = blank
json = require "json"

function dofile (filename)
    local f = loadfile(script_path.."/"..filename)
    return f()
end

test_file_names = get_test_file_names()
NUMBER_OF_TESTS = #test_file_names
ch = fiber.channel(NUMBER_OF_TESTS)
for i = 1, NUMBER_OF_TESTS do
    fiber.create(
        function ()
            --box.sql.execute("pragma vdbe_debug=true")
            local err, result = pcall(dofile, test_file_names[i])
            if err then
                print(test_file_names[i])
                print("ERROR "..json.encode(err))
            end
            ch:put(1)
        end
    )
end

for i = 1, NUMBER_OF_TESTS do
    ch:get()
end

test.finish_test = finish_test
test.plan = plan
test.fail = fail
test.ok = ok
test.io_write = io_write

test:do_test(
    "concurrency:4",
    function()
        -- If the previous workload did not crushed the server
        -- then the concurrency:4 is passed
        return 0
    end,
    0)

test:finish_test()
