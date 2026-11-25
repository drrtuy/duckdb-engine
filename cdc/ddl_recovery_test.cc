#include "ddl_recovery.h"

#include <functional>
#include <stdexcept>

using namespace myduck::cdc;

static void Need(bool value)
{
  if (!value) throw std::runtime_error("DDL recovery test failed");
}
static void Reject(const std::function<void()> &f)
{
  try { f(); } catch (const std::runtime_error &) { return; }
  throw std::runtime_error("ambiguous DDL recovery was accepted");
}
int main()
{
  for (const auto &op : {"CREATE", "ALTER", "DROP", "RENAME", "TRUNCATE"})
  {
    Need(RecoveryAction(op, true, "a", "", "b", "c", "b", "c") == DdlRecoveryAction::Confirm);
    Reject([&] { RecoveryAction(op, true, "a", "", "b", "c", "wrong", "c"); });
    Need(RecoveryAction(op, false, "a", "", "", "", "a", "") == DdlRecoveryAction::Execute);
  }
  Need(RecoveryAction("DROP", false, "a", "", "", "", "", "") == DdlRecoveryAction::Confirm);
  Need(RecoveryAction("RENAME", false, "a", "", "", "", "", "a") == DdlRecoveryAction::Confirm);
  Need(RecoveryAction("TRUNCATE", false, "a", "", "", "", "a", "") == DdlRecoveryAction::Execute);
  Reject([] { RecoveryAction("CREATE", false, "", "", "", "", "a", ""); });
  Reject([] { RecoveryAction("ALTER", false, "a", "", "", "", "b", ""); });
  Reject([] { RecoveryAction("RENAME", false, "a", "", "", "", "a", "a"); });
  auto schema = " (id int, v varchar(80) DEFAULT ' AUTO_INCREMENT=19') ENGINE=DUCKDB";
  Need(SchemaDefinition(std::string("CREATE TABLE `t`") + schema + " AUTO_INCREMENT=42", "t") == schema);
  Need(SchemaDefinition(std::string("CREATE TABLE `a``b`") + schema, "a`b") == schema);
  Need(SchemaDefinition("CREATE TABLE `t` (id int) AUTO_INCREMENT=100 DEFAULT CHARSET=utf8mb4", "t") ==
                       " (id int) DEFAULT CHARSET=utf8mb4");
  Reject([] { SchemaDefinition("CREATE TABLE `wrong` (id int)", "t"); });
  Reject([] { SchemaDefinition("CREATE TABLE `t` (v varchar(20) DEFAULT 'a\\\\b')", "t"); });
  Reject([] { SchemaDefinition("CREATE TABLE `t` (v varchar(20) DEFAULT 'unterminated)", "t"); });
}
