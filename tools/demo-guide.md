# MiniSQL 验收演示指南

## 准备

```bash
cd /home/jy/minisql/build
cp ../tools/sql_30k.txt .
./bin/main
```

打开一个终端窗口，启动 main 后逐一输入以下命令。**每条 SQL 末尾必须有分号。**

---

## 第一阶段：基本操作

```
create database db0;
create database db1;
create database db2;
show databases;
use db0;
```

**预期**：show databases 列出 db0、db1、db2。

---

## 第二阶段：建表

```
create table account(
  id int,
  name char(16),
  balance float,
  primary key(id)
);
-- 注意：name 列暂不加 unique 约束，后面单独演示唯一约束和索引创建
show tables;
```

**预期**：`show tables` 显示 `account` 表。

> 框架为 primary key 自动创建 B+ 树索引。name 列未设 unique，后面演示手动建索引。

---

## 第三阶段：批量插入 3 万条

```
execfile "sql_30k.txt";
```

3 万条 INSERT，预计 2-4 分钟。控制台逐条显示 `Query OK, 1 row affected`。

**验证**：

```
select * from account where id = 29999;
```

应返回最后一条记录。再用：

```
select * from account where id = 0;
```

应返回第一条。两条都对即证明 30000 条全部插入成功。（全表扫描 `select * from account` 输出太多，不推荐。）

---

## 第四阶段：点查询

```
select * from account where id = 12500;
select * from account where id = 15000;
select * from account where name = "name05678";
```

记录 `name = "name05678"` 的执行时间 **t₁**。此时 name 列**没有索引**，是全表扫描，较慢。

**不等值查询**：

```
select * from account where id <> 29999;
select * from account where name <> "name00000";
```

---

## 第五阶段：多条件与投影

```
select id, name from account where id >= 10000 and id < 10020;
select name, balance from account where id <= 50 and balance > 50000;
select * from account where id < 20000 and name > "name10000";
```

记录最后一条的执行时间，记为 **t₅**。

---

## 第六阶段：唯一约束

**主键冲突**：

```
insert into account values(0, "dup_test", 100.0);
```

**预期**：`PRIMARY KEY` 冲突（id=0 已存在）。

**演示 UNIQUE 约束（单独建表）**：

```
create table t_uniq(id int unique, val int);
insert into t_uniq values(1, 100);
insert into t_uniq values(1, 200);
```

**预期**：第二次插入报 `UNIQUE` 约束冲突（id=1 已存在）。

```
drop table t_uniq;
```

---

## 第七阶段：索引效果对比（核心）

此时只有 id 列有主键索引（`pk_account`），name 列**无索引**。我们在 name 上建索引，对比前后查询速度。

**查 name 点查（无索引，全表扫描）**：

```
select * from account where name = "name05678";
```

记录时间 **t₁**。（3 万条全表扫描。）

**在 name 上创建索引**：

```
create index idx01 on account(name);
show indexes;
```

现在 `show indexes` 显示两个索引：`pk_account`（主键）和 `idx01`（name 列手动建的）。

**再次查 name 点查（走索引）**：

```
select * from account where name = "name05678";
```

记录时间 **t₂**。**预期 t₂ < t₁**——走索引比全表扫描快得多。

**查 name 范围（走索引）**：

```
select * from account where name > "name14500" and name < "name14600";
```

记录时间 **t₃**。同样是走索引。

**多条件查（id 索引 + name 索引，很快）**：

```
select * from account where id < 20000 and name < "name10000";
```

记录时间 **t₄**。id 和 name 都有索引。

**删除和回插（验证索引维护）**：

```
delete from account where name = "name02345";
select * from account where name = "name02345";
insert into account values(99998, "name02345", 500.0);
select * from account where name = "name02345";
```

**删除索引后对比**：

```
drop index idx01;
select * from account where name = "name05678";
```

记录时间 **t₅**。**预期 t₅ > t₂**——索引被删，退回全表扫描。

---

## 第八阶段：更新与删除

```
update account set balance = 88888.88 where name = "name05678";
select * from account where name = "name05678";
```

验证 balance 已变为 88888.88。

```
delete from account where name = "name05678";
select * from account where name = "name05678";
```

应返回空。

```
delete from account;
select * from account where id = 0;
```

应返回空——全表已清空。

```
drop table account;
show tables;
```

account 应消失。

---

## 第九阶段：设计思路介绍

按架构从下到上：
1. Disk Manager → 多 Extent 位图管理，逻辑/物理页号映射
2. Buffer Pool Manager → LRU 淘汰 + free_list 两级调度
3. Record Manager → Slotted-page 堆表，Row/Field/Schema 序列化
4. Index Manager → 磁盘 B+ 树，二分查找，分裂/合并
5. Catalog Manager → 元数据持久化，每表/每索引独立数据页
6. Planner & Executor → 火山模型算子

**亮点**：B+ 树合并方向参考 CMU 修正（删除时的关键 bug fix），max_size 溢出保护，自编 47 个边界测试。

---

## 时间控制

| 阶段 | 时长 |
|------|------|
| 基本操作 + 建表 | 2 min |
| 批量插入（execfile） | 2-4 min |
| 点查 + 多条件 | 3 min |
| 约束 + 索引对比 | 3 min |
| 更新删除 | 2 min |
| 设计介绍 | 3-5 min |
| 提问 | 5 min |
| **总计** | **~20 min** |

---

## 注意事项

1. **每条 SQL 必须以分号结尾**
2. **execfile 前确保已 use db0**
3. **索引演示用 name 列**（建表时 name 不带 unique，`create index idx01 on account(name)` 手动建索引）
4. **展示失败场景**：插入重复主键、违反 unique 约束——都会打印明确的错误信息
