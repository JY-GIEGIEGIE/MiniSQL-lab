项目验收流程
进行基本操作演示；
简要介绍整个系统的设计思路，若实现了额外的功能或是性能优化可以一并介绍，介绍的时候最好能够体现出整体系统设计的亮点之处；
模块提问抽查，针对一些实现细节做提问。如果操作演示与思路介绍顺利则简化。
基本操作示例
创建数据库db0、db1、db2，并列出所有的数据库
在db0数据库上创建数据表account，表的定义如下：
考察SQL执行以及数据插入操作：
执行数据库文件sql.txt，向表中插入
100000
条记录（分10次插入，每次插入10000条，至少插入30000条）
参考SQL数据，由脚本自动生成：
验收数据.zip
(910 KB) 这里课程组给的数据失效了，我们可能需要自己设计脚本
批量执行时，所有sql执行完显示总的执行时间
执行全表扫描select * from account，验证插入的数据是否正确（要求输出查询到100000条记录）
考察点查询操作：
select * from account where id = ?
select * from account where balance = ?
select * from account where name = "name56789"，此处记录执行时间t 1
​
 
select * from account where id <> ?
select * from account where balance <> ?
select * from account where name <> ?
考察多条件查询与投影操作：
select id, name from account where balance >= ? and balance < ?
select name, balance from account where balance > ? and id <= ?
select * from account where id < 12515000 and name > "name14500"
select * from account where id < 12500200 and name < "name00100"，此处记录执行时间
t 5
​
 
考察唯一约束：
insert into account values(?, ?, ?)，提示PRIMARY KEY约束冲突或UNIQUE约束冲突
考察索引的创建删除操作、记录的删除操作以及索引的效果：
create index idx01 on account(name)
select * from account where name = "name56789"，此处记录执行时间
t 2
​
 
，要求
t 2<t 1
​
 
select * from account where name = "name45678"，此处记录执行时间
t 3
​
 
select * from account where id < 12500200 and name < "name00100"，此处记录执行时间
t 6
​
 
，比较t 5和t 6
​
 
delete from account where name = "name45678"
insert into account values(?, "name45678", ?)
drop index idx01
执行(c)的语句，此处记录执行时间t4，要求t3<t4
​
 
考察更新操作：
update account set id = ?, balance = ? where name = "name56789";
 并通过select操作验证记录被更新
考察删除操作：
delete from account where balance = ?，并通过select操作验证记录被删除
delete from account，并通过select操作验证全表被删除
drop table account，并通过show tables验证该表被删除

