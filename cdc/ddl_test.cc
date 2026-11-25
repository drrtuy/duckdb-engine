#include "ddl.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using myduck::cdc::PlanDdl;

namespace
{
void Check(bool value, const std::string &label)
{
  if (!value) throw std::runtime_error("test assertion failed: " + label);
}
void Good(const std::string &input, const std::string &expected, const std::string &operation,
          const std::string &table = "t", const std::string &next = "")
{
  const auto plan = PlanDdl(input, "app", "app");
  Check(plan.sql == expected, input + " -> " + plan.sql);
  Check(plan.operation == operation && plan.table == table && plan.new_table == next, input);
}
void Bad(const std::string &input, const std::string &context = "app", const std::string &allowed = "app")
{
  bool failed = false;
  try { PlanDdl(input, context, allowed); }
  catch (const std::runtime_error &) { failed = true; }
  Check(failed, "accepted: " + input);
}
void Supported()
{
  Good("create table if not exists app.t (id int(11) unsigned not null auto_increment, "
       "k int(11) not null default '0', c char(120) not null default '', "
       "pad char(60) not null default '', primary key (id), key k_1 (k)) "
       "engine=InnoDB default charset=utf8mb4 collate utf8mb4_general_ci",
       "CREATE TABLE IF NOT EXISTS `app`.`t` (`id` INT(11) UNSIGNED NOT NULL AUTO_INCREMENT, "
       "`k` INT(11) NOT NULL DEFAULT '0', `c` CHAR(120) NOT NULL DEFAULT '', "
       "`pad` CHAR(60) NOT NULL DEFAULT '', PRIMARY KEY (`id`), KEY `k_1` (`k`)) "
       "ENGINE=DuckDB DEFAULT CHARSET=utf8mb4 COLLATE=`utf8mb4_general_ci`", "CREATE");
  Good("CREATE TABLE t (id BIGINT PRIMARY KEY, a decimal(20,5) default -12.50, "
       "b datetime(6) default NULL, c varchar(64) null, d blob, index ix (c,id)) ENGINE=DuckDB",
       "CREATE TABLE `app`.`t` (`id` BIGINT PRIMARY KEY, `a` DECIMAL(20,5) DEFAULT -12.50, "
       "`b` DATETIME(6) DEFAULT NULL, `c` VARCHAR(64) NULL, `d` BLOB, KEY `ix` (`c`, `id`)) ENGINE=DuckDB",
       "CREATE");
  Good("DROP TABLE IF EXISTS app.t", "DROP TABLE IF EXISTS `app`.`t`", "DROP");
  Good("RENAME TABLE t TO app.next", "RENAME TABLE `app`.`t` TO `app`.`next`", "RENAME", "t", "next");
  Good("truncate t", "TRUNCATE TABLE `app`.`t`", "TRUNCATE");
  Good("TRUNCATE TABLE app.t", "TRUNCATE TABLE `app`.`t`", "TRUNCATE");
  Good("alter table t add x integer null", "ALTER TABLE `app`.`t` ADD COLUMN `x` INTEGER NULL", "ALTER");
  Good("ALTER TABLE t ADD COLUMN x VARCHAR(12) NOT NULL DEFAULT 'it''s'",
       "ALTER TABLE `app`.`t` ADD COLUMN `x` VARCHAR(12) NOT NULL DEFAULT 'it''s'", "ALTER");
  Good("ALTER TABLE t DROP x", "ALTER TABLE `app`.`t` DROP COLUMN `x`", "ALTER");
  Good("ALTER TABLE t DROP COLUMN `PRIMARY`", "ALTER TABLE `app`.`t` DROP COLUMN `PRIMARY`", "ALTER");
  Good("CREATE TABLE `t``x` (`c``d` TEXT DEFAULT 'x''); DROP TABLE app.t; --')",
       "CREATE TABLE `app`.`t``x` (`c``d` TEXT DEFAULT 'x''); DROP TABLE app.t; --') ENGINE=DuckDB",
       "CREATE", "t`x");
  Good("CREATE TABLE t (x TEXT DEFAULT '/* harmless */ # -- ;', y BOOL DEFAULT TRUE, z INT DEFAULT +02)",
       "CREATE TABLE `app`.`t` (`x` TEXT DEFAULT '/* harmless */ # -- ;', `y` BOOL DEFAULT TRUE, "
       "`z` INT DEFAULT 02) ENGINE=DuckDB", "CREATE");
  Check(PlanDdl("DROP TABLE app.t", "other", "app").sql == "DROP TABLE `app`.`t`", "qualified context");
  Check(PlanDdl("DROP TABLE app.t", "", "app").table == "t", "empty qualified context");
  Check(PlanDdl("DROP TABLE `a``b`.`t``b`", "a`b", "a`b").sql ==
        "DROP TABLE `a``b`.`t``b`", "database escaping");
  const std::vector<std::string> types = {
      "TINYINT(1)", "SMALLINT", "MEDIUMINT UNSIGNED", "INT", "INTEGER", "BIGINT", "FLOAT", "DOUBLE",
      "DECIMAL", "NUMERIC(65,30)", "BOOL", "BOOLEAN", "DATE", "YEAR", "DATETIME",
      "TIME(6)", "CHAR", "CHAR(255)", "VARCHAR(65535)", "BINARY(255)", "VARBINARY(65535)",
      "TINYBLOB", "BLOB", "MEDIUMBLOB", "LONGBLOB", "TINYTEXT", "TEXT", "MEDIUMTEXT", "LONGTEXT"};
  for (const auto &type : types)
    Check(!PlanDdl("CREATE TABLE t (x " + type + ")", "app", "app").sql.empty(), type);
}
void Rejected()
{
  const std::vector<std::string> cases = {
      "", " ", ";", "DROP TABLE t;", "DROP TABLE t; DROP DATABASE app", "DROP TABLE t -- x",
      "DROP TABLE t--x", "DROP TABLE t # x", "DROP /*x*/ TABLE t", "DROP /*!50000 TABLE */ t",
      "DROP /*M! TABLE */ t", "DROP TABLE t\nSELECT 1", "DROP TABLE t, u", "DROP TEMPORARY TABLE t",
      "DROP DATABASE app", "TRUNCATE TABLE t CASCADE", "RENAME TABLE t TO u, x TO y",
      "RENAME TABLE t TO other.u", "RENAME TABLE other.t TO u", "DROP TABLE other.t",
      "DROP TABLE mysql.user", "DROP TABLE information_schema.tables", "DROP TABLE sys.t",
      "DROP TABLE performance_schema.t", "DROP TABLE APP.t", "DROP TABLE app.t.x",
      "DROP TABLE `app.t`", "DROP TABLE `../t`", "DROP TABLE `t `", "DROP TABLE ``",
      "DROP TABLE `#mysql50#t`", "DROP TABLE `#mysql50#mysql`.user",
      "DROP TABLE \"t\"", "DROP TABLE `unterminated", "DROP TABLE `t\\`", "DROP TABLE té",
      "CREATE OR REPLACE TABLE t (x INT)", "CREATE TEMPORARY TABLE t (x INT)", "CREATE TABLE t LIKE u",
      "CREATE TABLE t AS SELECT 1", "CREATE TABLE t (x INT) AS SELECT 1", "CREATE TABLE t ()",
      "CREATE TABLE t (x INT,)", "CREATE TABLE t (x INT) PARTITION BY HASH(x) PARTITIONS 2",
      "CREATE TABLE t (x INT) ENGINE=MyISAM", "CREATE TABLE t (x INT) ENGINE=InnoDB ENGINE=DuckDB",
      "CREATE TABLE t (x INT) DATA DIRECTORY='/tmp'", "CREATE TABLE t (x INT) TABLESPACE ts",
      "CREATE TABLE t (x INT) DEFAULT CHARSET=latin1", "CREATE TABLE t (x INT) COLLATE=foo()",
      "CREATE TABLE t (x INT) DEFAULT CHARSET=utf8mb4 DEFAULT CHARSET=utf8mb4",
      "CREATE TABLE t (x INT) COLLATE=foo COLLATE=bar", "CREATE TABLE t (x INT COMMENT 'x')",
      "CREATE TABLE t (x INT, X INT)", "CREATE TABLE t (x INT, PRIMARY KEY(x), PRIMARY KEY(x))",
      "CREATE TABLE t (x INT PRIMARY KEY, PRIMARY KEY(x))", "CREATE TABLE t (x INT PRIMARY KEY PRIMARY KEY)",
      "CREATE TABLE t (x INT, KEY ix(y))", "CREATE TABLE t (x INT, PRIMARY KEY(x,x))",
      "CREATE TABLE t (x INT, KEY ix(x), KEY IX(x))", "CREATE TABLE t (x INT, KEY PRIMARY(x))",
      "CREATE TABLE t (x INT, KEY ix(x(2)))", "CREATE TABLE t (x INT, KEY ix(x DESC))",
      "CREATE TABLE t (x INT, FOREIGN KEY(x) REFERENCES other.t(x))",
      "CREATE TABLE t (x INT REFERENCES other.t(x))", "CREATE TABLE t (x INT CHECK(x>0))",
      "CREATE TABLE t (x INT, CHECK(x))", "CREATE TABLE t (x INT, CONSTRAINT c PRIMARY KEY(x))",
      "CREATE TABLE t (x INT, UNIQUE KEY ix(x))", "CREATE TABLE t (x INT GENERATED ALWAYS AS (1))",
      "CREATE TABLE t (x INT AS (1))", "CREATE TABLE t (x INT DEFAULT (1))",
      "CREATE TABLE t (x INT DEFAULT (SELECT 1))", "CREATE TABLE t (x TEXT DEFAULT load_file('/tmp/x'))",
      "CREATE TABLE t (x TIMESTAMP)", "ALTER TABLE t ADD COLUMN x TIMESTAMP NULL",
      "ALTER TABLE t ADD COLUMN x TIMESTAMP DEFAULT '2020-01-01 00:00:00'",
      "CREATE TABLE t (x TIMESTAMP DEFAULT CURRENT_TIMESTAMP)", "CREATE TABLE t (x INT DEFAULT rand())",
      "CREATE TABLE t (x INT DEFAULT abs(1))", "CREATE TABLE t (x INT DEFAULT 1+1)",
      "CREATE TABLE t (x INT DEFAULT 0x10)", "CREATE TABLE t (x INT DEFAULT 1e2)",
      "CREATE TABLE t (x TEXT DEFAULT _utf8mb4'x')", "CREATE TABLE t (x TEXT DEFAULT \"x\")",
      "CREATE TABLE t (x TEXT DEFAULT 'x\\y')", "CREATE TABLE t (x TEXT DEFAULT 'unterminated)",
      "CREATE TABLE t (x TEXT DEFAULT 'x' 'y')", "CREATE TABLE t (x INT DEFAULT NULL NOT NULL)",
      "CREATE TABLE t (x INT NULL NULL)", "CREATE TABLE t (x INT DEFAULT 1 DEFAULT 2)",
      "CREATE TABLE t (x INT AUTO_INCREMENT AUTO_INCREMENT)", "CREATE TABLE t (x TEXT AUTO_INCREMENT)",
      "CREATE TABLE t (x INT AUTO_INCREMENT DEFAULT 1)", "CREATE TABLE t (x INT PRIMARY KEY NULL)",
      "CREATE TABLE t (x INT, y INT AUTO_INCREMENT, z INT AUTO_INCREMENT)",
      "CREATE TABLE t (x GEOMETRY)", "CREATE TABLE t (x ENUM('a'))", "CREATE TABLE t (x JSON)",
      "CREATE TABLE t (x INT(0))", "CREATE TABLE t (x INT(256))", "CREATE TABLE t (x INT(-1))",
      "CREATE TABLE t (x VARCHAR)", "CREATE TABLE t (x VARCHAR(0))", "CREATE TABLE t (x CHAR(256))",
      "CREATE TABLE t (x VARCHAR(65536))", "CREATE TABLE t (x DECIMAL(66,2))",
      "CREATE TABLE t (x DECIMAL(5,6))", "CREATE TABLE t (x DECIMAL(65,31))",
      "CREATE TABLE t (x TIMESTAMP(7))", "CREATE TABLE t (x DATE(1))", "CREATE TABLE t (x TEXT UNSIGNED)",
      "CREATE TABLE t (x DOUBLE(5,2))", "CREATE TABLE t (x INT ZEROFILL)",
      "CREATE TABLE t (x INT) UNION=(other.t)", "ALTER TABLE t ADD COLUMN x INT NOT NULL",
      "ALTER TABLE t ADD COLUMN x INT NOT NULL DEFAULT NULL", "ALTER TABLE t ADD x INT FIRST",
      "ALTER TABLE t ADD x INT AFTER y", "ALTER TABLE t ADD x INT, ADD y INT",
      "ALTER TABLE t ADD x INT PRIMARY KEY", "ALTER TABLE t ADD x INT AUTO_INCREMENT",
      "ALTER TABLE t ADD KEY ix(x)", "ALTER TABLE t DROP PRIMARY KEY", "ALTER TABLE t DROP PRIMARY",
      "ALTER TABLE t DROP KEY ix", "ALTER TABLE t DROP INDEX ix", "ALTER TABLE t DROP FOREIGN KEY fk",
      "ALTER TABLE t DROP CONSTRAINT c", "ALTER TABLE t DROP CHECK c", "ALTER TABLE t DROP PARTITION p",
      "ALTER TABLE t DROP COLUMN x CASCADE", "ALTER TABLE t DROP x, DROP y", "ALTER TABLE t ENGINE=InnoDB",
      "ALTER TABLE t MODIFY x INT", "ALTER TABLE t CHANGE x y INT", "ALTER TABLE t RENAME TO u",
      "CREATE PROCEDURE p() SELECT 1", "CALL p()", "SELECT load_file('/tmp/x')", "USE app",
      "GRANT ALL ON app.* TO x", "SET GLOBAL local_infile=1", "INSTALL SONAME 'x'"};
  for (const auto &input : cases) Bad(input);
  for (const auto &db : {"mysql", "MYSQL", "information_schema", "performance_schema", "sys", "", "../app", "app "})
    Bad("DROP TABLE t", db, db);
  Bad("DROP TABLE t", "other");
  Bad("DROP TABLE t", "");
  Bad("DROP TABLE t", "APP");
  Bad("DROP TABLE t" + std::string(1, '\0'));
  Bad("CREATE TABLE t (x TEXT DEFAULT 'a" + std::string(1, '\0') + "b')");
  Bad("DROP TABLE `" + std::string(65, 'x') + "`");
  Bad(std::string(1024 * 1024 + 1, ' '));
  std::string tokens;
  for (unsigned n = 0; n < 33000; ++n) tokens += "x ";
  Bad(tokens);
  Bad("CREATE TABLE t (x INT DEFAULT " + std::string(66, '9') + ")");
  Bad("CREATE TABLE t (x VARCHAR(9999999999999999999999999999999))");
}
void Boundaries()
{
  const std::string valid = "CREATE TABLE `t``x` (id INT PRIMARY KEY, x VARCHAR(30) DEFAULT 'a''b') ENGINE=InnoDB";
  for (size_t n = 0; n < valid.size(); ++n)
  {
    try { PlanDdl(valid.substr(0, n), "app", "app"); }
    catch (const std::runtime_error &) {}
  }
  for (unsigned byte = 0; byte < 256; ++byte)
  {
    const std::string insertion(1, static_cast<char>(byte));
    if (byte < 32 || byte > 126 || byte == '\\')
      Bad("CREATE TABLE t (x TEXT DEFAULT 'a" + insertion + "b')");
    for (size_t pos = 0; pos <= valid.size(); ++pos)
    {
      std::string mutated = valid;
      mutated.insert(pos, insertion);
      std::string canonical;
      try { canonical = PlanDdl(mutated, "app", "app").sql; }
      catch (const std::runtime_error &) { continue; }
      Check(PlanDdl(canonical, "app", "app").sql == canonical, "unstable canonical SQL");
    }
  }
}
}

int main()
{
  try
  {
    Supported();
    Rejected();
    Boundaries();
    std::cout << "CDC DDL tests passed\n";
    return 0;
  }
  catch (const std::exception &error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
