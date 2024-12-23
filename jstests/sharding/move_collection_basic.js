/**
 * Tests for basic functionality of the move collection feature.
 *
 * @tags: [
 *  requires_fcv_80,
 *  featureFlagReshardingImprovements,
 *  featureFlagMoveCollection,
 *  # TODO (SERVER-87812) Remove multiversion_incompatible tag
 *  multiversion_incompatible,
 *  assumes_balancer_off,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

(function() {
'use strict';

var st = new ShardingTest({mongos: 1, shards: 2});

// TODO (SERVER-96071): Delete this helper once the namespace length extension for tracked
// collections is backported to v8.0.
function is80orBelow(mongos) {
    const res =
        mongos.getDB("admin").system.version.find({_id: "featureCompatibilityVersion"}).toArray();
    return MongoRunner.compareBinVersions(res[0].version, "8.1") < 0;
}

const dbName = 'db';
const collName = 'foo';
const ns = dbName + '.' + collName;
let mongos = st.s0;
let shard0 = st.shard0.shardName;
let shard1 = st.shard1.shardName;

let cmdObj = {moveCollection: ns, toShard: shard0};

// Fail if collection is not tracked.
assert.commandFailedWithCode(mongos.adminCommand(cmdObj), ErrorCodes.NamespaceNotFound);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: shard0}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

// Fail if collection is sharded.
assert.commandFailedWithCode(mongos.adminCommand(cmdObj), ErrorCodes.NamespaceNotFound);

const unsplittableCollName = "foo_unsplittable";
const unsplittableCollNs = dbName + '.' + unsplittableCollName;
assert.commandWorked(st.s.getDB(dbName).runCommand({create: unsplittableCollName}));

// Fail if missing required field toShard.
assert.commandFailedWithCode(mongos.adminCommand({moveCollection: unsplittableCollNs}),
                             ErrorCodes.IDLFailedToParse);

// Fail if command called on shard.
assert.commandFailedWithCode(
    st.shard0.adminCommand({moveCollection: unsplittableCollNs, toShard: shard1}),
    ErrorCodes.CommandNotFound);

const coll = mongos.getDB(dbName)[unsplittableCollName];
for (let i = -25; i < 25; ++i) {
    assert.commandWorked(coll.insert({oldKey: i}));
}
assert.eq(50, st.rs0.getPrimary().getCollection(unsplittableCollNs).countDocuments({}));

// move to non-primary shard.
assert.commandWorked(mongos.adminCommand({moveCollection: unsplittableCollNs, toShard: shard1}));

// Should have unsplittable set to true
let configDb = mongos.getDB('config');
let unshardedColl = configDb.collections.findOne({_id: unsplittableCollNs});
assert.eq(unshardedColl.unsplittable, true);
let unshardedChunk = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
assert.eq(1, unshardedChunk.length);

assert.eq(50, st.rs1.getPrimary().getCollection(unsplittableCollNs).countDocuments({}));
assert.eq(0, st.rs0.getPrimary().getCollection(unsplittableCollNs).countDocuments({}));

const metrics = st.config0.getDB('admin').serverStatus({}).shardingStatistics.moveCollection;

assert.eq(metrics.countStarted, 1);
assert.eq(metrics.countSameKeyStarted, undefined);
assert.eq(metrics.countSucceeded, 1);
assert.eq(metrics.countFailed, 0);
assert.eq(metrics.countCanceled, 0);

// move to primary shard.
assert.commandWorked(mongos.adminCommand({moveCollection: unsplittableCollNs, toShard: shard0}));
unshardedColl = configDb.collections.findOne({_id: unsplittableCollNs});
assert.eq(unshardedColl.unsplittable, true);
unshardedChunk = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
assert.eq(1, unshardedChunk.length);

assert.eq(0, st.rs1.getPrimary().getCollection(unsplittableCollNs).countDocuments({}));
assert.eq(50, st.rs0.getPrimary().getCollection(unsplittableCollNs).countDocuments({}));

// Successfully move a collection with a long namespace below 255.
const collLength = is80orBelow(mongos) ? 200 : 250;
const longCollName = 'a'.repeat(collLength);
const longNs = dbName + "." + longCollName;
assert.commandWorked(mongos.getDB(dbName).createCollection(longCollName));
assert.commandWorked(mongos.adminCommand({moveCollection: longNs, toShard: shard1}));
st.stop();
})();
