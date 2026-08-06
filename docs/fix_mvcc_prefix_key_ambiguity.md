# 修复：MVCC 键编码前缀歧义导致 Prewrite 写冲突检查失效（第四轮）

## 1. 现象

在前三轮修复（lease read 追平闸门、`RAFT_SYNC=true`、TSO 预留窗口扩大）之后，
`txn_bank_chaos_test.sh 60` 仍然失败：

```text
提交成功: 16698   提交失败/冲突: 62895
审计通过: 1   审计重试: 2   审计违规: 211
最终总额: 15998（期望 16000）
FAIL: 总额不变量被破坏
```

同时离线检查全部通过，看起来"账面干净"却总额不守恒：

```text
PASS: 全部节点 lock CF 为空、write CF 无悬空 start_ts
write CF 共 160172 条：Put=33418 Delete=0 Rollback=126754
```

## 2. 定位过程：直接审计 write CF 的完整提交历史

由于 `snapshot_interval_s=3600` 期间没有 GC/compaction，write CF 保留了
本次压测的**全部** 33418 条 Put 提交记录。把节点 8200 的 RocksDB 拷出后，
按 `commit_ts` 升序重放每一笔事务（同一 `commit_ts` 的两条记录属于同一笔
转账，作为一组原子应用），逐组计算 16 个账户的总额：

```text
txn groups 16702    sum-changing anomalies 339
10:08:16.035 sum 16000 -> 15996 [('acct_1', 992, ...), ('acct_5', 1015, ...)]
10:08:16.372 sum 15996 -> 15995 [('acct_1', 961, ...), ('acct_7', 1022, ...)]
10:08:16.486 sum 15995 -> 15994 [('acct_1', 973, ...), ('acct_3', 951, ...)]
...
```

两个决定性事实：

1. **339 次总额跳变，每一次的参与账户都包含 `acct_1`**；
2. 第一次跳变发生在 `18:08:16.03`（ts 高 18 位换算的物理时间），
   而混沌脚本的第一次 `kill -9` 在约 `18:08:20`——
   **丢失更新在无故障、单一稳定 leader 时就已经持续发生**。

这直接排除了 Raft 选主/lease/日志持久性/TSO 方向，问题只可能出在
MVCC 本身对 `acct_1` 这个 key 的处理上。`acct_1` 的特殊性一目了然：
它是 `acct_10`…`acct_15` 的**前缀**。

## 3. 根因：EncodeKey 无分隔符，前缀 key 的版本区间被扩展 key 打断

MVCC 键编码（`src/storage/mvcc_codec.h`）：

```text
EncodeKey(user_key, ts) = user_key + big_endian(~ts)    // 8 字节，无分隔符
```

本系统的混合时间戳 `ts = 物理毫秒 << 18`，当前年代的 ts 高字节约为
`0x06`，因此 `~ts` 的首字节约为 `0xF9`。于是 write CF 中：

```text
"acct_1"  的版本键 = "acct_1" + 0xF9 ...        （尾部首字节 ≈ 0xF9）
"acct_10" 的版本键 = "acct_1" + '0'(0x30) + ...
"acct_15" 的版本键 = "acct_1" + '5'(0x35) + ...
```

按字节序，**acct_10…acct_15 的所有版本都排在 acct_1 自己的版本之前**，
且都落在"以 `acct_1` 为前缀"的键区间内。

`MvccTxn::SeekWrite(key, ts)` 原实现假设一个 key 的所有版本是连续区间：

```cpp
for (it->Seek(MvccCodec::EncodeKey(key, ts)); it->Valid(); it->Next()) {
  ...
  if (user_key != key) {
    break;   // ← 以为已越过该 key 的全部版本
  }
  ...
}
```

当 `ts = UINT64_MAX` 时（`~ts` 为 8 个 `0x00`），seek 落点是
`"acct_1"+0x00×8`，即整个前缀区间的最前端——第一条命中的必然是
`acct_10` 的最新版本，`user_key != key` 直接 `break`，
**返回"该 key 无任何提交记录"**。`GetTxnRecord` 的 seek 起点同样是
`UINT64_MAX`，同样中招。

### 3.1 由此引发的连锁故障（全部只影响 acct_1 这类前缀 key）

| 调用点 | 依赖 | 失效后果 |
|---|---|---|
| `Prewrite` 第 1 步 `SeekWrite(key, UINT64_MAX)` | 最新 commit_ts ≥ start_ts 则拒绝 | **写冲突检查恒通过**：基于陈旧读的事务照样提交 → 丢失更新，总额漂移（本次 -2，过程中最深 -139） |
| `Prewrite` 第 2 步 `GetTxnRecord` | rollback 墓碑挡迟到 Prewrite | 墓碑失效，被回滚事务可"复活" |
| `Commit` 锁不在时 `GetTxnRecord` | 已提交则幂等成功 | 幂等失效，重试的 Commit 误报 `TxnNotFound` |
| `Rollback` / `CheckTxnStatus` `GetTxnRecord` | 已提交事务不可回滚 | 可能对已提交事务再写墓碑（脏历史） |

注意**快照读不受影响**：`Get(key, snapshot_ts)` 的 seek 尾部是
`~snapshot_ts ≈ 0xF9…`，字节序天然越过 `0x30…0x35` 的扩展键块，
落点正确。这解释了为什么审计读出的是"真实但已被破坏"的总额——
状态机本身被丢失更新写坏了，而不是读到了陈旧副本。

也解释了此前所有离线检查为何全部 PASS：丢失更新在时间戳上没有任何
异常特征（新事务的 `start_ts` 确实大于被覆盖版本的 `commit_ts`，
只是它"没看见"也"没检查到"那个版本），lock CF 也确实清空了。

## 4. 修复

`src/storage/mvcc_txn.cc`：`SeekWrite` 与 `GetTxnRecord` 在迭代中
遇到"前缀扩展键"（如查 `acct_1` 撞到 `acct_10` 的版本）时不再 `break`，
而是 `Seek(VersionRangeEnd(该扩展键))` 整块跳过后继续：

```cpp
// user_key 是 key 的前缀扩展（如查 "acct_1" 撞到 "acct_10" 的版本）。
// 编码无分隔符，扩展键的版本块会插在 key 自己的版本区间之前/之中，
// 迭代时不能 break，要整块跳过。
bool IsPrefixExtension(std::string_view user_key, const std::string &key) {
  return user_key.size() > key.size() &&
         user_key.substr(0, key.size()) == key;
}

// SeekWrite / GetTxnRecord 循环内：
if (user_key != key) {
  if (IsPrefixExtension(user_key, key)) {
    it->Seek(MvccCodec::VersionRangeEnd(user_key));  // 跳过整个扩展键版本块
    continue;
  }
  break; // 已越过该 key 的全部版本
}
```

要点：

- **不改键编码格式**，磁盘数据、`mvcc_db_check`、单测全部兼容；
- 跳过用一次 `Seek` 而不是逐条 `Next`，避免热点 key 的冲突检查
  线性扫过扩展键的全部历史版本（压测末期有数万条）；
- 每个扩展键块最多一次 `Seek`，本测试场景（acct_1 下辖 6 个扩展键）
  单次检查最多多 6 次 seek，代价可忽略；
- `GetTxnRecord` 中 `commit_ts < start_ts` 的提前终止判断移到
  确认 `user_key == key` 之后——扩展键上解出的 `commit_ts` 是
  另一个 key 的时间戳，不能用来做终止判断。

### 4.1 跳过边界的安全性说明

`VersionRangeEnd(ext)` = `ext + 0xFF×8 + 0x00`，大于 ext 的全部版本。
理论上若目标 key 存在某版本的 `~commit_ts` 首字节恰好等于扩展键的
下一个字符（如 `0x30`，对应 `commit_ts` 高字节 `0xCF`、约 10^19 量级），
该版本会被一并跳过；混合时间戳在可预见的年代高字节固定为 `0x06`
（`~ts` 首字节 `0xF9`），不会落入 `0x30..0x39` 区间，实际不可达。
根本性的消除需要改用带终止符/memcomparable 的键编码（见第 6 节）。

## 5. 验证

用 RocksDB 搭最小复现（先提交 `acct_1@(s=10,c=20)`，再提交
`acct_10@(s=30,c=40)` 制造扩展键版本块）：

| 检查 | 修复前 | 修复后 |
|---|---|---|
| `Prewrite("acct_1", start_ts=15)` 应报 WriteConflict | FAIL（kind=0，冲突漏检） | PASS |
| `Commit("acct_1", 10, 20)` 重试应幂等成功 | FAIL（误报锁被他人持有） | PASS |
| `Get("acct_1", snap=45)` 应读到 1000 | FAIL（被上一步残锁挡住） | PASS |
| `Rollback` 墓碑应挡住迟到 Prewrite | FAIL（墓碑漏检） | PASS |

复验命令（预期：审计违规 0，最终总额 16000，三副本 lock CF 为空）：

```bash
cd build && make kv_server bank_chaos_test mvcc_db_check
bash ../tests/chaos/txn_bank_chaos_test.sh 60
```

## 6. 遗留与建议

1. **键编码本质缺陷**：任何"key A 是 key B 前缀"的组合都存在歧义。
   本修复在迭代层面消除了正确性问题；长期建议改为 TiKV 式
   memcomparable 编码（8 字节分组 + padding + 计数标记字节），
   使任意 user_key 的版本区间严格连续且前缀无关。该改动会改变
   磁盘格式并需要同步修改 `mvcc_codec` 单测与 `Scan` 的游标逻辑。
2. `Scan` 的输出顺序在前缀 key 场景下并非严格字典序
   （`acct_10` 会先于 `acct_1` 被吐出），结果集完整、不影响正确性；
   若上层依赖有序输出，随键编码重构一并解决。
3. 前三轮修复（lease read 追平闸门、`RAFT_SYNC=true`、TSO 预留窗口）
   针对的是真实存在的其他故障模式，全部保留。
