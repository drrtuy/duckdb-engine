#include "ha_table_exists_compat.h"

#include <initializer_list>
#include <stdexcept>

namespace
{

const LEX_CSTRING database= {"database", 8};
const LEX_CSTRING table= {"table", 5};
handlerton engine{};
bool exists;

void require(bool condition)
{
  if (!condition)
    throw std::runtime_error("ha_table_exists compatibility test failed");
}

bool check_arguments(THD *thd, const LEX_CSTRING *db, const LEX_CSTRING *name,
                     LEX_CUSTRING *version, handlerton **hton, bool *sequence)
{
  require(!thd && db == &database && name == &table && !version && !sequence);
  require(hton && !*hton);
  *hton= &engine;
  return exists;
}

bool with_partition_engine(THD *thd, const LEX_CSTRING *db,
                            const LEX_CSTRING *name, LEX_CUSTRING *version,
                            LEX_CSTRING *partition_engine, handlerton **hton,
                            bool *sequence)
{
  require(!partition_engine);
  return check_arguments(thd, db, name, version, hton, sequence);
}

bool without_partition_engine(THD *thd, const LEX_CSTRING *db,
                               const LEX_CSTRING *name, LEX_CUSTRING *version,
                               handlerton **hton, bool *sequence)
{
  return check_arguments(thd, db, name, version, hton, sequence);
}

bool with_flags(THD *thd, const LEX_CSTRING *db, const LEX_CSTRING *name,
                 LEX_CUSTRING *version, handlerton **hton, bool *sequence,
                 uint flags)
{
  require(flags == 0);
  return check_arguments(thd, db, name, version, hton, sequence);
}

}

int main()
{
  auto check= [](auto table_exists)
  {
    handlerton *hton= nullptr;
    require(myduck::ha_table_exists_compat(table_exists, nullptr, &database,
                                           &table, &hton) == exists);
    require(hton == &engine);
  };
  for (bool value : {false, true})
  {
    exists= value;
    check(&with_partition_engine);
    check(&without_partition_engine);
    check(&with_flags);
  }
}
