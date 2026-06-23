# MiniSQL 验收演示指南

## 准备

```bash
cd /home/jy/minisql/build
rm -rf databases        # 必须！清旧数据库，防止段错误
cp ../tools/sql_30k.txt .
./bin/main
```

**每条 SQL 末尾必须有分号。每次启动 main 前必须 `rm -rf databases`。**

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
show tables;
```

**预期**：`show tables` 显示 `account`。name 列不带 unique（后面单独演示唯一约束和索引创建）。

---

## 第三阶段：创建索引后批量插入

先建索引，再插数据——索引随插入自动填充。

```
create index idx01 on account(name);
show indexes;
execfile "sql_30k.txt";
```

**预期**：`show indexes` 显示 `pk_account` 和 `idx01`。execfile 逐条插入 3 万条。

---

## 第四阶段：验证插入

```
select * from account where id = 29999;
select * from account where id = 0;
```

首尾两条都对即证明 3 万条全部插入成功。

---

## 第五阶段：点查询

```
select * from account where id = 12500;
select * from account where id = 15000;
select * from account where name = "name05678";
```

记录 `name = "name05678"` 的执行时间 **t₁**。（name 列已有 idx01 索引，走索引很快。）

不等值查询：

```
select * from account where id <> 29999;
select * from account where name <> "name00000";
```

---

## 第六阶段：多条件与投影

```
select id, name from account where id >= 10000 and id < 10020;
select name, balance from account where id <= 50;
select * from account where id < 20000 and name > "name10000";
select * from account where id < 20000 and name < "name00100";
```

记录最后一条的执行时间 **t₅**。

---

## 第七阶段：唯一约束

**主键冲突**：

```
insert into account values(0, "dup_test", 100.0);
```

**预期**：`PRIMARY KEY` 冲突。

**UNIQUE 约束演示**：

```
create table t_uniq(id int unique, val int);
insert into t_uniq values(1, 100);
insert into t_uniq values(1, 200);
```

**预期**：第二次插入报 `UNIQUE` 约束冲突。

```
drop table t_uniq;
```

---

## 第八阶段：索引效果验证

name 列的 idx01 索引在插入数据前已创建。演示删除索引后性能变化。

**有索引时点查**：

```
select * from account where name = "name05678";
```

记录时间 **t₂**。（走 idx01 索引。）

```
select * from account where name = "name02345";
```

记录时间 **t₃**。

**有索引时多条件查**：

```
select * from account where id < 20000 and name < "name00100";
```

记录时间 **t₆**。比较 t₅ 和 t₆。

**删除和回插**：

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

记录时间 **t₄**。**预期 t₃ < t₄**（索引被删，退回全表扫描变慢）。

---

## 第九阶段：更新与删除

```
update account set id = 99997, balance = 88888.88 where name = "name05678";
select * from account where name = "name05678";
```

验证记录被更新。

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

## 设计思路介绍（自由发挥）

按架构从下到上：
1. Disk Manager → 多 Extent 位图管理，逻辑/物理页号映射
2. Buffer Pool Manager → LRU 淘汰 + free_list 两级调度
3. Record Manager → Slotted-page 堆表，Row/Field/Schema 序列化
4. Index Manager → 磁盘 B+ 树，分裂/合并
5. Catalog Manager → 元数据持久化
6. Planner & Executor → 火山模型算子

**亮点**：B+ 树合并方向 CMU 参考修正，max_size 溢出保护。

---

## 注意事项

1. **每条 SQL 以分号结尾**
2. **每次启动 main 前 `rm -rf databases`**
3. **execfile 前确保已 use db0**
4. name 列对应的 id：name00000→id=0, name00100→id=100, name02345→id=2345, name05678→id=5678, name10000→id=10000, name14500→id=14500
