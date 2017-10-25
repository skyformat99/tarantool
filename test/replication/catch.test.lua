env = require('test_run')
test_run = env.new()
engine = test_run:get_cfg('engine')

box.schema.user.grant('guest', 'read,write,execute', 'universe')

net_box = require('net.box')
errinj = box.error.injection

box.schema.user.grant('guest', 'replication')
test_run:cmd("create server replica with rpl_master=default, script='replication/replica.lua'")
test_run:cmd("start server replica")
test_run:cmd("switch replica")

test_run:cmd("switch default")
s = box.schema.space.create('test', {engine = engine});
-- vinyl does not support hash index
index = s:create_index('primary', {type = (engine == 'vinyl' and 'tree' or 'hash') })

test_run:cmd("switch replica")
fiber = require('fiber')
while box.space.test == nil do fiber.sleep(0.01) end
test_run:cmd("switch default")
test_run:cmd("stop server replica")

-- insert values on the master while replica is stopped and can't fetch them
for i=1,100 do s:insert{i, 'this is test message12345'} end

-- Check that replica doesn't accept requests before catching up
-- with the master. To check that we inject sleep into the master
-- relay_send function and execute a 'select' on the replica.
-- We should see all data inserted while the replica was down,
-- not just a part of it.

errinj.set("ERRINJ_RELAY_TIMEOUT", 0.001)

test_run:cmd("start server replica")
test_run:cmd("switch replica")

box.space.test:count() -- 100

test_run:cmd("switch default")

errinj.set("ERRINJ_RELAY_TIMEOUT", 0)

-- cleanup
test_run:cmd("stop server replica")
test_run:cmd("cleanup server replica")
box.space.test:drop()
box.schema.user.revoke('guest', 'replication')
box.schema.user.revoke('guest', 'read,write,execute', 'universe')

