import logging
import random
import threading
import time
from collections import Counter

import pytest
from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

node_with_backoff = cluster.add_instance(
    "node_with_backoff",
    macros={"cluster": "test_cluster"},
    main_configs=["configs/config.d/backoff_mutation_policy.xml"],
    with_zookeeper=True,
)

node_no_backoff = cluster.add_instance(
    "node_no_backoff",
    macros={"cluster": "test_cluster"},
    with_zookeeper=True,
)

REPLICATED_POSPONE_MUTATION_LOG = (
    "According to exponential backoff policy, put aside this log entry"
)
POSPONE_MUTATION_LOG = (
    "According to exponential backoff policy, do not perform mutations for the part"
)

all_nodes = [node_with_backoff, node_no_backoff]


def prepare_cluster(use_replicated_table):
    for node in all_nodes:
        node.query("DROP TABLE IF EXISTS test_mutations SYNC")

    engine = (
        "ReplicatedMergeTree('/clickhouse/{cluster}/tables/test/test_mutations', '{instance}')"
        if use_replicated_table
        else "MergeTree()"
    )

    for node in all_nodes:
        node.query(f"CREATE TABLE test_mutations(x UInt32) ENGINE {engine} ORDER BY x")
        node.query("INSERT INTO test_mutations SELECT * FROM system.numbers LIMIT 10")


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster.start()
        yield cluster

    finally:
        cluster.shutdown()


@pytest.mark.parametrize(
    ("node, found_in_log"),
    [
        (
            node_with_backoff,
            True,
        ),
        (
            node_no_backoff,
            False,
        ),
    ],
)
def test_exponential_backoff_with_merge_tree(started_cluster, node, found_in_log):
    prepare_cluster(False)

    # Executing incorrect mutation.
    node.query(
        "ALTER TABLE test_mutations DELETE WHERE x IN (SELECT x  FROM notexist_table) SETTINGS allow_nondeterministic_mutations=1"
    )

    assert node.contains_in_log(POSPONE_MUTATION_LOG) == found_in_log
    node.rotate_logs()

    time.sleep(5)
    node.query("KILL MUTATION WHERE table='test_mutations'")
    # Check that after kill new parts mutations are postponing.
    node.query(
        "ALTER TABLE test_mutations DELETE WHERE x IN (SELECT x  FROM notexist_table) SETTINGS allow_nondeterministic_mutations=1"
    )

    assert node.contains_in_log(POSPONE_MUTATION_LOG) == found_in_log


def test_exponential_backoff_with_replicated_tree(started_cluster):

    prepare_cluster(True)

    node_no_backoff.query(
        "ALTER TABLE test_mutations DELETE WHERE x IN (SELECT x  FROM notexist_table) SETTINGS allow_nondeterministic_mutations=1"
    )

    time.sleep(5)
    assert node_no_backoff.contains_in_log(REPLICATED_POSPONE_MUTATION_LOG) == False
    assert node_with_backoff.contains_in_log(REPLICATED_POSPONE_MUTATION_LOG) == True


@pytest.mark.parametrize(
    ("node"),
    [
        (node_with_backoff),
    ],
)
def test_exponential_backoff_create_dependent_table(started_cluster, node):

    prepare_cluster(False)

    node.query("INSERT INTO test_mutations SELECT * FROM system.numbers LIMIT 10")
    # Executing incorrect mutation.
    node.query(
        "ALTER TABLE test_mutations DELETE WHERE x IN (SELECT x  FROM dep_table) SETTINGS allow_nondeterministic_mutations=1"
    )
    time.sleep(5)
    # Creating dependent table for mutation.
    node.query("CREATE TABLE dep_table(x UInt32) ENGINE MergeTree() ORDER BY x")

    time.sleep(5)
    assert node.query("SELECT count() FROM system.mutations WHERE is_done=0") == "0\n"
