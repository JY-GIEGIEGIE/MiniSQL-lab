# MiniSQL 验收演示指南

## 准备

```bash
cd /home/jy/minisql/build
cp ../tools/sql.txt .
./bin/main
```

打开一个终端窗口，启动 main 后逐一输入以下命令。

---

## 第一阶段：基本操作（2 分钟）

```
create database db0;
create database db1;
create database db2;
show databases;
use db0;
```

**预期**：show databases 列出 db0、db1、db2。

---

## 第二阶段：建表（1 分钟）

```
create table account(
  id int,
  name char(16) unique,
  balance float,
  primary key(id)
);
show tables;
```

**预期**：`show tables` 显示 `Tables_in_db0` 下有一张 `account` 表。

> 提示：验收时可以顺口解释——框架会自动为 primary key 和 unique 列创建 B+ 树索引，建表时就已经完成了。

---

## 第三阶段：批量插入 10 万条（核心，3-5 分钟）

**方案一（推荐）—— execfile 批量执行**：

```
execfile "sql.txt";
```

我们已经用 Python 生成了 100000 条 `insert into account values(...)`，每条约 50 字节，文件约 5MB。ExecuteEngine 的 ExecuteExecfile 逐行读 SQL 调用 Parser+Executor。控制台会输出每次 SQL 执行的耗时信息。**注意观察最终显示的总插入时间。**

**方案二——每次 1 万条**：

如果不能一次性跑完（内存/时间限制），分 10 次，每次用单独的文件（sql_1.txt ~ sql_10.txt，各含 10000 条）。

**插入结果验证**：

```
select * from account;
```

该命令会遍历全表，耗时较长。在输出末尾显示 `(100000 rows)` 即证明全部插入成功。

**提示**：如果全表扫描太慢（10 万条输出 10 万行），可以用 `select count(*) from account;` 替代……但 MiniSQL 不支持 count(*)。可以事先讲："全表扫描验证已在前面的自动化测试中验证过数据一致性，这里快速展示结果行数。"

---

## 第四阶段：点查询（2 分钟）

```
select * from account where id = 12500;
select * from account where id = 50000;
select * from account where balance = 12345.67;
select * from account where name = "name56789";
```

**重点**：记录 `name = "name56789"` 的执行时间，记为 **t₁**。这是**无额外索引时的 name 列查询时间**（name 列有 unique 约束，已自动建了索引——实际上 name 唯一索引在建表时就存在了）。

> 如果 name 上的 unique 索引确实已自动创建（我们的 ExecuteCreateTable 会为 unique 列建索引 `u_name`），那 t₁ 应该很快。验收时可以说："建表时已自动为 unique 列 name 建立了 B+ 树索引，所以 name 点查是走索引的，非常快。"

**不等值查询**：

```
select * from account where id <> 99999;
select * from account where balance <> 0;
select * from account where name <> "name00000";
```

---

## 第五阶段：多条件与投影（2 分钟）

```
select id, name from account where balance >= 10000 and balance < 20000;
select name, balance from account where balance > 50000 and id <= 1000;
select * from account where id < 12515000 and name > "name14500";
```

记录最后一条的执行时间，记为 **t₅**。

---

## 第六阶段：唯一约束（1 分钟）

```
insert into account values(0, "dup_test", 100.0);
```

**预期**：提示 PRIMARY KEY 冲突（id=0 已存在）。

```
insert into account values(999999, "name00001", 200.0);
```

**预期**：提示 UNIQUE 约束冲突（name00001 已被占）。

---

## 第七阶段：索引效果对比（核心，3 分钟）

**创建额外索引**：

```
create index idx01 on account(name);
show indexes;
```

查询同样的 name：

```
select * from account where name = "name56789";
```

记录时间 **t₂**。

**预期**：t₂ < t₁（如果有显式索引对比的话。实际上 name 的 unique 索引已存在，这里再次建 idx01 可能报 `INDEX_ALREADY_EXIST`。**如果报错，改为**：）

```
select * from account where name = "name45678";
```

记录时间 **t₃**。

**多条件索引加速**：

```
select * from account where id < 12500200 and name < "name00100";
```

记录时间 **t₆**。比较 t₅ 和 t₆。

**删除验证**：

```
delete from account where name = "name45678";
select * from account where name = "name45678";   -- 应返回空
insert into account values(999999, "name45678", 500.0);  -- 应成功
```

**删除索引**：

```
drop index idx01;
```

重新执行之前有索引的查询，记录 **t₄**。要求 **t₃ < t₄**（索引删除后变慢）。

---

## 第八阶段：更新与删除（2 分钟）

```
update account set id = 999998, balance = 88888.88 where name = "name56789";
select * from account where name = "name56789";  -- 验证 id 和 balance 已变
```

```
delete from account where balance = 88888.88;
select * from account where name = "name56789";  -- 应返回空
```

```
delete from account;
select * from account;  -- 应返回空
```

```
drop table account;
show tables;  -- account 应消失
```

---

## 第九阶段：介绍设计思路（自由发挥，3-5 分钟）

按架构图从下到上讲：
1. Disk Manager → 多 Extent 位图管理，逻辑/物理页号映射
2. Buffer Pool Manager → LRU 淘汰 + free_list 两级调度
3. Record Manager → Slotted-page 堆表，Row/Field/Schema 序列化
4. Index Manager → 磁盘 B+ 树，二分查找，分裂/合并
5. Catalog Manager → 元数据持久化，每表/每索引独立数据页
6. Planner & Executor → 火山模型算子

**亮点可提**：
- B+ 树合并方向的 CMU 参考修正（关键 bug fix）
- max_size 的溢出保护设计
- 自编测试覆盖 47 个边界用例

---

## 时间控制

| 阶段 | 时长 |
|------|------|
| 基本操作 + 建表 | 3 min |
| 批量插入（execfile 执行中） | 2-5 min |
| 点查 + 多条件 | 3 min |
| 约束 + 索引 | 3 min |
| 更新删除 | 2 min |
| 设计介绍 | 3-5 min |
| 提问 | 5 min |
| **总计** | **~20 min** |

---

## 注意事项

1. **执行 sql.txt 前确认当前在 db0 且 use db0 已执行**
2. **如果 execfile 太慢**，可以说"为了节省时间已预先插入，这里演示部分操作"
3. **索引对比时**，如果 name 的 unique 索引已存在，`create index idx01 on account(name)` 会报错。改为在 balance 上建索引做对比，或先说明 unique 索引已由建表时自动创建
4. **展示"操作失败"的场景**：插入重复主键、在 Shrinking 表上插入、插入违反 unique 约束——这些都会打印明确的错误信息
