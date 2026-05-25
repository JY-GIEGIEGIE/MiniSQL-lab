### 项目验收流程
1. 进行基本操作演示；
2. 简要介绍整个系统的设计思路，若实现了额外的功能或是性能优化可以一并介绍，介绍的时候最好能够体现出整体系统设计的亮点之处；
3. 模块提问抽查，针对一些实现细节做提问，考察项目是否是由小组独立完成的**<font style="color:#E8323C;">（可能会问得非常深入，请做好充分准备）</font>**

### 基本操作示例
1. 创建数据库`db0`、`db1`、`db2`，并列出所有的数据库
2. 在`db0`数据库上创建数据表`account`，表的定义如下：

```sql
create table account(
  id int, 
  name char(16) unique, 
  balance float, 
  primary key(id)
);

-- Note: 在实现中自动为UNIQUE列建立B+树索引的情况下，
--       这里的NAME列不加UNIQUE约束，UNIQUE约束将另行考察。
--			（NAME列创建索引的时候，不需要限制只有UNIQUE列才能建立索引）
```

3. 考察SQL执行以及数据插入操作：
    1. 执行数据库文件`sql.txt`，向表中插入$ 100000 $条记录（分$ 10 $次插入，每次插入$ 10000 $条，至少插入$ 30000 $条）
        1. 参考SQL数据，由脚本自动生成：[验收数据.zip](https://www.yuque.com/attachments/yuque/0/2023/zip/29437275/1686492221764-b7ba2711-03b6-4a69-882e-de26d227ce9b.zip)
        2. 批量执行时，所有sql执行完显示总的执行时间
    2. 执行全表扫描`select * from account`，验证插入的数据是否正确（要求输出查询到$ 100000 $条记录）
4. 考察点查询操作：
    1. `select * from account where id = ?`
    2. `select * from account where balance = ?`
    3. `select * from account where name = "name56789"`，此处记录执行时间$ t_1 $
    4. `select * from account where id <> ?`
    5. `select * from account where balance <> ?`
    6. `select * from account where name <> ?`
5. 考察多条件查询与投影操作：
    1. `select id, name from account where balance >= ? and balance < ?`
    2. `select name, balance from account where balance > ? and id <= ?`
    3. `select * from account where id < 12515000 and name > "name14500"`
    4. `select * from account where id < 12500200 and name < "name00100"`，此处记录执行时间$ t_5 $
6. 考察唯一约束：
    1. `insert into account values(?, ?, ?)`，提示PRIMARY KEY约束冲突或UNIQUE约束冲突
7. 考察索引的创建删除操作、记录的删除操作以及索引的效果：
    1. `create index idx01 on account(name)`
    2. `select * from account where name = "name56789"`，此处记录执行时间$ t_2 $，要求$ t_2<t_1 $
    3. `select * from account where name = "name45678"`，此处记录执行时间$ t_3 $
    4. `select * from account where id < 12500200 and name < "name00100"`，此处记录执行时间$ t_6 $，比较$ t_5 $和$ t_6 $
    5. `delete from account where name = "name45678"`
    6. `insert into account values(?, "name45678", ?)`
    7. `drop index idx01`
    8. 执行(c)的语句，此处记录执行时间$ t_4 $，要求$ t_3<t_4 $
8. 考察更新操作：
    1. `update account set id = ?, balance = ? where name = "name56789";`

 并通过`select`操作验证记录被更新

9. 考察删除操作：
    1. `delete from account where balance = ?`，并通过`select`操作验证记录被删除
    2. `delete from account`，并通过`select`操作验证全表被删除
    3. `drop table account`，并通过`show tables`验证该表被删除



