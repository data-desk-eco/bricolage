#include <stdio.h>
#include <string.h>
#include "sqlite3.h"
static int auth(void*u,int op,const char*a,const char*b,const char*c,const char*d){
  if(op==SQLITE_INSERT)printf("auth insert %s.%s\n",c,a);return SQLITE_OK;}
static int con(sqlite3*db,void*p,int n,const char*const*a,sqlite3_vtab**v,char**e){
  sqlite3_declare_vtab(db,"create table x(key,parent,brief hidden)");*v=sqlite3_malloc(sizeof(**v));memset(*v,0,sizeof(**v));return SQLITE_OK;}
static int dis(sqlite3_vtab*v){sqlite3_free(v);return SQLITE_OK;}
static int best(sqlite3_vtab*v,sqlite3_index_info*i){printf("xBestIndex\n");return SQLITE_OK;}
static int open_(sqlite3_vtab*v,sqlite3_vtab_cursor**c){*c=sqlite3_malloc(sizeof(**c));memset(*c,0,sizeof(**c));return SQLITE_OK;}
static int close_(sqlite3_vtab_cursor*c){sqlite3_free(c);return SQLITE_OK;}
static int filt(sqlite3_vtab_cursor*c,int n,const char*s,int argc,sqlite3_value**a){printf("xFilter\n");return SQLITE_OK;}
static int next(sqlite3_vtab_cursor*c){return SQLITE_OK;}
static int eof(sqlite3_vtab_cursor*c){return 1;}
static int col(sqlite3_vtab_cursor*c,sqlite3_context*x,int i){return SQLITE_OK;}
static int rid(sqlite3_vtab_cursor*c,sqlite3_int64*r){return SQLITE_OK;}
static sqlite3_module m={0,con,con,best,dis,dis,open_,close_,filt,next,eof,col,rid};
int main(){sqlite3*db;sqlite3_open(":memory:",&db);sqlite3_set_authorizer(db,auth,0);
 sqlite3_create_module(db,"run",&m,0);
 sqlite3_exec(db,"create temp table fill(key text primary key,parent text);create table company(key)",0,0,0);
 sqlite3_stmt*s;printf("-- prepare\n");
 sqlite3_prepare_v2(db,"insert into fill select r.key,r.parent from run('b') r join company t on r.key=t.key",-1,&s,0);
 printf("-- step\n");sqlite3_step(s);sqlite3_finalize(s);return 0;}
