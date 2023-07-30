- scale-2-shard-moving.drawio.svg
  - 在 h 迁移到 t2 上的过程中，如果有读取请求到来，会发生什么？

- scale-3-shard-split-merge.drawio.svg
  - split: https://forums.foundationdb.org/t/keyspace-partitions-performance/168/4
    - write bandwidth (write hotspot): 250KB/s
    - size 125MB ~ 500MB, smaller grows and shrinks with database size

- data moving
  - split/merge
  - balance bytes stored across the storage servers

- load balancer
  - client track latency of each servers: https://forums.foundationdb.org/t/keyspace-partitions-performance/168/4
  - rate keeper 限速

- recovery 流程：
  - resolver 怎么 recovery conflict: https://forums.foundationdb.org/t/technical-overview-of-the-database/135/24
    All ongoing transactions by clients will be aborted with transaction_too_old if a recovery happens before they commit. Resolvers only need to conflict check transactions started after the recovery, which is exactly the same as transactions started as of when the resolver was created.
  - 关于 anti-quorum: https://forums.foundationdb.org/t/question-about-sigmod21-paper/3240/6?u=walter
    Antiquorums is a “feature” in FDB that lets you specify the number of TLogs which can not respond to a commit, and the ack will still be sent to the client that the commit is durable. The idea behind it was that dropping the slowest TLog from each commit would significantly improve tail latency. I called it a 'feature" because it’s essentially broken, and no one uses it on their clusters (outside of simulation tests), because it can lead to terrible behavior. One TLog is permitted to become unboundedly far behind, because it can always be the TLog dropped from the quorum. Storage servers can still be assigned this unboundedly behind TLog as their preferred TLog, and thus become unboundedly far behind themself. Ratekeeper will eventually throttle the cluster to zero assuming that storage servers aren’t catching up because of an overload, whereas it’s actually because their TLog isn’t feeding them any newer commits, because the TLog isn’t receiving the commits itself.
    So the recovery logic was written this way to support antiquorums, but no one uses antiquorums, and we don’t recommend that anyone use antiquorums. Thus, we didn’t discuss antiquorums in the paper, but we still described the actual recovery logic of FDB in the paper, which was written to support antiquorums, and that’s how we ended up here.
  - 为什么 KCV 前的不需要？
    有人问了相同的问题：https://forums.foundationdb.org/t/why-there-is-no-document-explaining-foundationdb-internals/2287/4?u=walter
    在：https://github.com/apple/foundationdb/blob/main/documentation/sphinx/source/architecture.rst 里提到了：
    >  In contrast, Transaction Logs save the logs of committed transactions, and we need to ensure all previously committed transactions are durable and retrievable by storage servers. That is, for any transactions that the Commit Proxies may have sent back commit response, their logs are persisted in multiple Log Servers (e.g., three servers if replication degree is 3).
    Finally, a recovery will fast forward time by 90 seconds, which would abort any in-progress client transactions with transaction_too_old error. During retry, these client transactions will find the new generation of transaction system and commit.

    ``commit_result_unknown`` error: If a recovery happened while a transaction is committing (i.e., a commit proxy has sent mutations to transaction logs). A client would have received commit_result_unknown, and then retried the transaction. It’s completely permissible for FDB to commit both the first attempt, and the second retry, as commit_result_unknown means the transaction may or may not have committed. This is why it’s strongly recommended that transactions should be idempotent, so that they handle commit_result_unknown correctly.
  - Data Team
    - Copysets: Reducing the Frequency of Data Loss in Cloud Storage
    - The number of machines in each team is based on the replication mode; the total number of teams increases with the size of the cluster.
    -  A failure of a StorageServer triggers DataDistributor to move data from teams containing the failed
process to other healthy teams.
    - 先修只剩 1 个副本的
    - 再修只剩 2 个副本的
    - 最后是 balancer



