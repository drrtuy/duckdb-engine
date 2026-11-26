#pragma once

struct st_mysql_daemon;
struct st_mysql_sys_var;
struct st_mysql_show_var;

namespace myduck
{
namespace cdc
{

#ifdef WITH_DUCKDB_CDC
int Init(void *);
int Deinit(void *);
void Shutdown();
extern st_mysql_daemon daemon;
extern st_mysql_sys_var *variables[];
extern st_mysql_show_var status[];
#else
inline void Shutdown() {}
#endif

}
}
