/*
  Another Python Sqlite Wrapper

  This wrapper aims to be the minimum necessary layer over SQLite 3
  itself.

  It assumes we are running as 32 bit int with a 64 bit long long type
  available.

  Copyright (C) 2004-2008 Roger Binns <rogerb@rogerbinns.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any
  damages arising from the use of this software.
 
  Permission is granted to anyone to use this software for any
  purpose, including commercial applications, and to alter it and
  redistribute it freely, subject to the following restrictions:
 
  1. The origin of this software must not be misrepresented; you must
     not claim that you wrote the original software. If you use this
     software in a product, an acknowledgment in the product
     documentation would be appreciated but is not required.

  2. Altered source versions must be plainly marked as such, and must
     not be misrepresented as being the original software.

  3. This notice may not be removed or altered from any source
     distribution.
 
*/

/* Fight with setuptools over ndebug */
#ifdef APSW_NO_NDEBUG
#ifdef NDEBUG
#undef NDEBUG
#endif
#endif

/* SQLite amalgamation */
#ifdef APSW_USE_SQLITE_AMALGAMATION
/* See SQLite ticket 2554 */
#define SQLITE_API static
#define SQLITE_EXTERN static
#include APSW_USE_SQLITE_AMALGAMATION

/* Fight with SQLite over ndebug */
#ifdef APSW_NO_NDEBUG
#ifdef NDEBUG
#undef NDEBUG
#endif
#endif

#else
/* SQLite 3 headers */
#include "sqlite3.h"
#endif

#if SQLITE_VERSION_NUMBER < 3006002
#error Your SQLite version is too old.  It must be at least 3.6.2
#endif

/* system headers */
#include <assert.h>
#include <stdarg.h>

/* Get the version number */
#include "apswversion.h"

/* Python headers */
#include <Python.h>
#include <pythread.h>
#include "structmember.h"

#ifdef APSW_TESTFIXTURES
/* Fault injection */
#define APSW_FAULT_INJECT(name,good,bad)          \
do {                                              \
  if(APSW_Should_Fault(#name))                    \
    {                                             \
      do { bad ; } while(0);                      \
    }                                             \
  else                                            \
    {                                             \
      do { good ; } while(0);                     \
    }                                             \
 } while(0)

static int APSW_Should_Fault(const char *);

/* Are we Python 2.x (x>=5) and doing 64 bit? - _LP64 is best way I can find as sizeof isn't valid in cpp #if */
#if  PY_VERSION_HEX>=0x02050000 && defined(_LP64) && _LP64
#define APSW_TEST_LARGE_OBJECTS
#endif

#else /* APSW_TESTFIXTURES */
#define APSW_FAULT_INJECT(name,good,bad)        \
  do { good ; } while(0)

#endif

/* The encoding we use with SQLite.  SQLite supports either utf8 or 16
   bit unicode (host byte order).  If the latter is used then all
   functions have "16" appended to their name.  The encoding used also
   affects how strings are stored in the database.  We use utf8 since
   it is more space efficient, and Python can't make its mind up about
   Unicode (it uses 16 or 32 bit unichars and often likes to use Byte
   Order Markers as well). */
#define STRENCODING "utf-8"


/* Various modules */

/* A module to augment tracebacks */
#include "traceback.c"

/* Make various versions of Python code compatible with each other */
#include "pyutil.c"

/* A list of pointers (used by Connection to keep track of Cursors) */
#include "pointerlist.c"

/* Exceptions we can raise */
#include "exceptions.c"

/* various utility functions and macros */
#include "util.c"

/* The module object */
static PyObject *apswmodule;


/* Some macros used for frequent operations */

#define CHECK_USE(e)                                                \
  { if(self->inuse)                                                                                 \
      {    /* raise exception if we aren't already in one */                                                                         \
           if (!PyErr_Occurred())                                                                                                    \
             PyErr_Format(ExcThreadingViolation, "You are trying to use the same object concurrently in two threads which is not allowed."); \
           return e;                                                                                                                 \
      }                                                                                                                              \
  }

#define CHECK_CLOSED(connection,e) \
{ if(!connection->db) { PyErr_Format(ExcConnectionClosed, "The connection has been closed"); return e; } }

#define APSW_BEGIN_ALLOW_THREADS \
  do { \
      assert(self->inuse==0); self->inuse=1; \
      Py_BEGIN_ALLOW_THREADS

#define APSW_END_ALLOW_THREADS \
     Py_END_ALLOW_THREADS; \
     assert(self->inuse==1); self->inuse=0; \
  } while(0)



/* CALLBACK INFO */

/* details of a registered function passed as user data to sqlite3_create_function */
typedef struct _funccbinfo 
{
  struct _funccbinfo *next;       /* we use a linked list */
  char *name;                     /* ascii function name which we uppercased */
  PyObject *scalarfunc;           /* the function to call for stepping */
  PyObject *aggregatefactory;     /* factory for aggregate functions */
} funccbinfo;

/* a particular aggregate function instance used as sqlite3_aggregate_context */
typedef struct _aggregatefunctioncontext 
{
  PyObject *aggvalue;             /* the aggregation value passed as first parameter */
  PyObject *stepfunc;             /* step function */
  PyObject *finalfunc;            /* final function */
} aggregatefunctioncontext;

static funccbinfo *freefunccbinfo(funccbinfo *);

typedef struct Connection Connection; /* forward declaration */

typedef struct _vtableinfo
{
  PyObject *datasource;           /* object with create/connect methods */
  Connection *connection;         /* the Connection this is registered against so we don't
				     have to have a global table mapping sqlite3_db* to
				     Connection* */
} vtableinfo;


/* CONNECTION TYPE */

struct Connection { 
  PyObject_HEAD
  sqlite3 *db;                    /* the actual database connection */
  const char *filename;           /* utf8 filename of the database */
  int co_linenumber;              /* line number of allocation */
  PyObject *co_filename;          /* filename of allocation */

  unsigned inuse;                 /* track if we are in use preventing concurrent thread mangling */

  pointerlist dependents;         /* tracking cursors & blobs belonging to this connection */
  struct StatementCache *stmtcache;      /* prepared statement cache */

  funccbinfo *functions;          /* linked list of registered functions */

  /* registered hooks/handlers (NULL or callable) */
  PyObject *busyhandler;     
  PyObject *rollbackhook;
  PyObject *profile;
  PyObject *updatehook;
  PyObject *commithook;           
  PyObject *progresshandler;      
  PyObject *authorizer;
  PyObject *collationneeded;

  /* if we are using one of our VFS since sqlite doesn't reference count them */
  PyObject *vfs;
};

static PyTypeObject ConnectionType;

/* CURSOR TYPE */

typedef struct {
  PyObject_HEAD
  Connection *connection;          /* pointer to parent connection */

  unsigned inuse;                  /* track if we are in use preventing concurrent thread mangling */
  struct APSWStatement *statement; /* statement we are currently using */

  /* what state we are in */
  enum { C_BEGIN, C_ROW, C_DONE } status;

  /* bindings for query */
  PyObject *bindings;              /* dict or sequence */
  Py_ssize_t bindingsoffset;       /* for sequence tracks how far along we are when dealing with multiple statements */

  /* iterator for executemany, original query string */
  PyObject *emiter;
  PyObject *emoriginalquery;

  /* tracing functions */
  PyObject *exectrace;
  PyObject *rowtrace;
  
} APSWCursor;

static PyTypeObject APSWCursorType;



/* forward declarations */
static PyObject *APSWCursor_close(APSWCursor *self, PyObject *args);

/* buffer used in statement cache */
#include "apswbuffer.c"

/* The statement cache */
#include "statementcache.c"

/* Zeroblob and blob */
#include "blob.c"

#include "vtable.c"

/* CONNECTION CODE */

static void
Connection_internal_cleanup(Connection *self)
{
  if(self->filename)
    {
      PyMem_Free((void*)self->filename);
      self->filename=0;
    }

  Py_XDECREF(self->co_filename);
  self->co_filename=0;

  /* free functions */
  {
    funccbinfo *func=self->functions;
    while((func=freefunccbinfo(func)));
    self->functions=0;
  }

  Py_XDECREF(self->busyhandler);
  self->busyhandler=0;

  Py_XDECREF(self->rollbackhook);
  self->rollbackhook=0;

  Py_XDECREF(self->profile);
  self->profile=0;

  Py_XDECREF(self->updatehook);
  self->updatehook=0;

  Py_XDECREF(self->commithook);
  self->commithook=0;

  Py_XDECREF(self->progresshandler);
  self->progresshandler=0;
  
  Py_XDECREF(self->authorizer);
  self->authorizer=0;

  Py_XDECREF(self->collationneeded);
  self->collationneeded=0;

  Py_XDECREF(self->vfs);
  self->vfs=0;
}

/* Closes cursors and blobs belonging to this connection */
static PyObject *
Connection_close(Connection *self, PyObject *args)
{
  int res;
  pointerlist_visit plv;
  int force=0;

  if(!self->db)
    goto finally;

  CHECK_USE(NULL);

  assert(!PyErr_Occurred());

  if(!PyArg_ParseTuple(args, "|i:close(force=False)", &force))
    return NULL;

  for(pointerlist_visit_begin(&self->dependents, &plv);
      pointerlist_visit_finished(&plv);
      pointerlist_visit_next(&plv))
    {
      PyObject *closeres=NULL;
      PyObject *obj=(PyObject*)pointerlist_visit_get(&plv);

      closeres=Call_PythonMethodV(obj, "close", 1, "(i)", force);
      Py_XDECREF(closeres);
      if(!closeres)
        return NULL;
    }

  statementcache_free(self->stmtcache);
  self->stmtcache=0;

  APSW_BEGIN_ALLOW_THREADS
    APSW_FAULT_INJECT(ConnectionCloseFail, res=sqlite3_close(self->db), res=SQLITE_IOERR);
  APSW_END_ALLOW_THREADS;

  if (res!=SQLITE_OK) 
    {
      SET_EXC(res, self->db);
    }

  if(PyErr_Occurred())
    {
      AddTraceBackHere(__FILE__, __LINE__, "Connection.close", NULL);
    }

  /* note: SQLite ignores error returns from vtabDisconnect, so the
     database still ends up closed and we return an exception! */

  if(res!=SQLITE_OK)
      return NULL;

  self->db=0;

  Connection_internal_cleanup(self);

 finally:
  if(PyErr_Occurred())
    return NULL;
  Py_RETURN_NONE;
}

static void
Connection_dealloc(Connection* self)
{
  if(self->db)
    {
      int res;

      if(self->stmtcache)
        {
          statementcache_free(self->stmtcache);
          self->stmtcache=0;
        }

      APSW_BEGIN_ALLOW_THREADS
        APSW_FAULT_INJECT(DestructorCloseFail, res=sqlite3_close(self->db), res=SQLITE_IOERR);
      APSW_END_ALLOW_THREADS;
      self->db=0;

      if(res!=SQLITE_OK)
        {
          /* not allowed to clobber existing exception */
          PyObject *etype=NULL, *evalue=NULL, *etraceback=NULL, *utf8filename=NULL;
          PyErr_Fetch(&etype, &evalue, &etraceback);

          utf8filename=getutf8string(self->co_filename);
          
          PyErr_Format(ExcConnectionNotClosed, 
                       "apsw.Connection on \"%s\" at address %p, allocated at %s:%d. The destructor "
                       "has encountered an error %d closing the connection, but cannot raise an exception.",
                       self->filename?self->filename:"NULL", self,
                       PyBytes_AsString(utf8filename), self->co_linenumber,
                       res);
          
          apsw_write_unraiseable(NULL);
          Py_XDECREF(utf8filename);
          PyErr_Restore(etype, evalue, etraceback);
        }
    }

  /* Our dependents all hold a refcount on us, so they must have all
     released before this destructor could be called */
  assert(self->dependents.numentries==0);
  pointerlist_free(&self->dependents);

  Connection_internal_cleanup(self);

  Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject*
Connection_new(PyTypeObject *type, APSW_ARGUNUSED PyObject *args, APSW_ARGUNUSED PyObject *kwds)
{
    Connection *self;

    self = (Connection *)type->tp_alloc(type, 0);
    if (self != NULL) {
      /* Strictly speaking the memory was already zeroed.  This is
         just defensive coding. */
      self->db=0;
      self->inuse=0;
      self->filename=0;
      self->co_linenumber=0;
      self->co_filename=0;
      memset(&self->dependents, 0, sizeof(self->dependents));
      pointerlist_init(&self->dependents);
      self->stmtcache=0;
      self->functions=0;
      self->busyhandler=0;
      self->rollbackhook=0;
      self->profile=0;
      self->updatehook=0;
      self->commithook=0;
      self->progresshandler=0;
      self->authorizer=0;
      self->collationneeded=0;
      self->vfs=0;
    }

    return (PyObject *)self;
}

/* forward declaration so we can tell if it is one of ours */
static int apswvfs_xAccess(sqlite3_vfs *vfs, const char *zName, int flags, int *pResOut);

static int
Connection_init(Connection *self, PyObject *args, PyObject *kwds)
{
  static char *kwlist[]={"filename", "flags", "vfs", "statementcachesize", NULL};
  PyObject *hooks=NULL, *hook=NULL, *iterator=NULL, *hookargs=NULL, *hookresult=NULL;
  PyFrameObject *frame;
  char *filename=NULL;
  int res=0;
  int flags=SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  char *vfs=0;
  int statementcachesize=100;
  sqlite3_vfs *vfsused=0;

  if(!PyArg_ParseTupleAndKeywords(args, kwds, "es|izi:Connection(filename, flags=SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, vfs=None, statementcachesize=100)", kwlist, STRENCODING, &filename, &flags, &vfs, &statementcachesize))
    return -1;
  
  if(statementcachesize<0)
    statementcachesize=0;

  /* Technically there is a race condition as a vfs of the same name
     could be registered between our find and the open starting.
     Don't do that! */
  APSW_BEGIN_ALLOW_THREADS
    vfsused=sqlite3_vfs_find(vfs);
    res=sqlite3_open_v2(filename, &self->db, flags, vfs);
  APSW_END_ALLOW_THREADS;
  SET_EXC(res, self->db);  /* nb sqlite3_open always allocates the db even on error */
  
  if(res!=SQLITE_OK)
      goto pyexception;
    
  if(vfsused && vfsused->xAccess==apswvfs_xAccess)
    {
      PyObject *pyvfsused=(PyObject*)(vfsused->pAppData);
      Py_INCREF(pyvfsused);
      self->vfs=pyvfsused;
    }

  /* record where it was allocated */
  frame = PyThreadState_GET()->frame;
  self->co_linenumber=PyCode_Addr2Line(frame->f_code, frame->f_lasti);
  self->co_filename=frame->f_code->co_filename;
  Py_INCREF(self->co_filename);
  self->filename=filename;
  filename=NULL; /* connection has ownership now */

  /* get detailed error codes */
  sqlite3_extended_result_codes(self->db, 1);
  
  /* call connection hooks */
  hooks=PyObject_GetAttrString(apswmodule, "connection_hooks");
  if(!hooks)
    goto pyexception;

  hookargs=Py_BuildValue("(O)", self);
  if(!hookargs) goto pyexception;

  iterator=PyObject_GetIter(hooks);
  if(!iterator)
    {
      AddTraceBackHere(__FILE__, __LINE__, "Connection.__init__", "{s: i}", "connection_hooks", hooks);
      goto pyexception;
    }

  while( (hook=PyIter_Next(iterator)) )
    {
      hookresult=PyEval_CallObject(hook, hookargs);
      if(!hookresult) 
	goto pyexception;
      Py_DECREF(hook);
      Py_DECREF(hookresult);
    }

  if(!PyErr_Occurred())
    {
      res=0;
      self->stmtcache=statementcache_init(self->db, statementcachesize);
      goto finally;
    }

 pyexception:
  /* clean up db since it is useless - no need for user to call close */
  res= -1;
  sqlite3_close(self->db);
  self->db=0;
  Connection_internal_cleanup(self);

finally:
  if(filename) PyMem_Free(filename);
  Py_XDECREF(hookargs);
  Py_XDECREF(iterator);
  Py_XDECREF(hooks);
  Py_XDECREF(hook);
  Py_XDECREF(hookresult);
  return res;
}


static PyObject *
Connection_blobopen(Connection *self, PyObject *args)
{
  APSWBlob *apswblob=0;
  sqlite3_blob *blob=0;
  const char *dbname, *tablename, *column;
  long long rowid;
  int writing;
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);
  
  if(!PyArg_ParseTuple(args, "esesesLi:blobopen(database, table, column, rowid, rd_wr)", 
                       STRENCODING, &dbname, STRENCODING, &tablename, STRENCODING, &column, &rowid, &writing))
    return NULL;

  APSW_BEGIN_ALLOW_THREADS
    res=sqlite3_blob_open(self->db, dbname, tablename, column, rowid, writing, &blob);
  APSW_END_ALLOW_THREADS;

  PyMem_Free((void*)dbname);
  PyMem_Free((void*)tablename);
  PyMem_Free((void*)column);
  SET_EXC(res, self->db);
  if(res!=SQLITE_OK)
    return NULL;
  
  APSW_FAULT_INJECT(BlobAllocFails,apswblob=PyObject_New(APSWBlob, &APSWBlobType), (PyErr_NoMemory(), apswblob=NULL));
  if(!apswblob)
    {
      sqlite3_blob_close(blob);
      return NULL;
    }

  pointerlist_add(&self->dependents, apswblob);
  APSWBlob_init(apswblob, self, blob);
  return (PyObject*)apswblob;
}

static void APSWCursor_init(APSWCursor *, Connection *);

static PyObject *
Connection_cursor(Connection *self)
{
  APSWCursor* cursor = NULL;

  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  APSW_FAULT_INJECT(CursorAllocFails,cursor = PyObject_New(APSWCursor, &APSWCursorType), (PyErr_NoMemory(), cursor=NULL));
  if(!cursor)
    return NULL;

  /* incref me since cursor holds a pointer */
  Py_INCREF((PyObject*)self);
  pointerlist_add(&self->dependents, cursor);
  APSWCursor_init(cursor, self);
  
  return (PyObject*)cursor;
}

static PyObject *
Connection_setbusytimeout(Connection *self, PyObject *args)
{
  int ms=0;
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  if(!PyArg_ParseTuple(args, "i:setbusytimeout(millseconds)", &ms))
    return NULL;

  res=sqlite3_busy_timeout(self->db, ms);
  SET_EXC(res, self->db);
  if(res!=SQLITE_OK) return NULL;
  
  /* free any explicit busyhandler we may have had */
  Py_XDECREF(self->busyhandler);
  self->busyhandler=0;

  Py_RETURN_NONE;
}

static PyObject *
Connection_changes(Connection *self)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);
  return PyLong_FromLong(sqlite3_changes(self->db));
}

static PyObject *
Connection_totalchanges(Connection *self)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);
  return PyLong_FromLong(sqlite3_total_changes(self->db));
}

static PyObject *
Connection_getautocommit(Connection *self)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);
  if (sqlite3_get_autocommit(self->db))
    Py_RETURN_TRUE;
  Py_RETURN_FALSE;
}

static PyObject *
Connection_last_insert_rowid(Connection *self)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  return PyLong_FromLongLong(sqlite3_last_insert_rowid(self->db));
}

static PyObject *
Connection_complete(Connection *self, PyObject *args)
{
  char *statements=NULL;
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);
  
  if(!PyArg_ParseTuple(args, "es:complete(statement)", STRENCODING, &statements))
    return NULL;

  res=sqlite3_complete(statements);

  PyMem_Free(statements);

  if(res)
    {
      Py_INCREF(Py_True);
      return Py_True;
    }
  Py_INCREF(Py_False);
  return Py_False;
}

static PyObject *
Connection_interrupt(Connection *self)
{
  CHECK_CLOSED(self, NULL);

  sqlite3_interrupt(self->db);  /* no return value */
  Py_RETURN_NONE;
}

#ifdef EXPERIMENTAL
static PyObject *
Connection_limit(Connection *self, PyObject *args)
{
  int val=-1, res, id;
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);
  if(!PyArg_ParseTuple(args, "i|i", &id, &val))
    return NULL;

  res=sqlite3_limit(self->db, id, val);

  return PyLong_FromLong(res);
}
#endif

static void
updatecb(void *context, int updatetype, char const *databasename, char const *tablename, sqlite3_int64 rowid)
{
  /* The hook returns void. That makes it impossible for us to
     abort immediately due to an error in the callback */
  
  PyGILState_STATE gilstate;
  PyObject *retval=NULL;
  Connection *self=(Connection *)context;

  assert(self);
  assert(self->updatehook);
  assert(self->updatehook!=Py_None);

  gilstate=PyGILState_Ensure();

  if(PyErr_Occurred())
    goto finally;  /* abort hook due to outstanding exception */

  retval=PyObject_CallFunction(self->updatehook, "(iO&O&L)", updatetype, convertutf8string, databasename, convertutf8string, tablename, rowid);

 finally:
  Py_XDECREF(retval);
  PyGILState_Release(gilstate);
}

static PyObject *
Connection_setupdatehook(Connection *self, PyObject *callable)
{
  /* sqlite3_update_hook doesn't return an error code */
  
  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  if(callable==Py_None)
    {
      sqlite3_update_hook(self->db, NULL, NULL);
      callable=NULL;
      goto finally;
    }

  if(!PyCallable_Check(callable))
    {
      PyErr_Format(PyExc_TypeError, "update hook must be callable");
      return NULL;
    }

  sqlite3_update_hook(self->db, updatecb, self);

  Py_INCREF(callable);

 finally:

  Py_XDECREF(self->updatehook);
  self->updatehook=callable;

  Py_RETURN_NONE;
}

static void
rollbackhookcb(void *context)
{
  /* The hook returns void. That makes it impossible for us to
     abort immediately due to an error in the callback */
  
  PyGILState_STATE gilstate;
  PyObject *retval=NULL;
  Connection *self=(Connection *)context;

  assert(self);
  assert(self->rollbackhook);
  assert(self->rollbackhook!=Py_None);

  gilstate=PyGILState_Ensure();

  APSW_FAULT_INJECT(RollbackHookExistingError,,PyErr_NoMemory());

  if(PyErr_Occurred())
    goto finally;  /* abort hook due to outstanding exception */

  retval=PyEval_CallObject(self->rollbackhook, NULL);

 finally:
  Py_XDECREF(retval);
  PyGILState_Release(gilstate);
}

static PyObject *
Connection_setrollbackhook(Connection *self, PyObject *callable)
{
  /* sqlite3_rollback_hook doesn't return an error code */
  
  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  if(callable==Py_None)
    {
      sqlite3_rollback_hook(self->db, NULL, NULL);
      callable=NULL;
      goto finally;
    }

  if(!PyCallable_Check(callable))
    {
      PyErr_Format(PyExc_TypeError, "rollback hook must be callable");
      return NULL;
    }

  sqlite3_rollback_hook(self->db, rollbackhookcb, self);

  Py_INCREF(callable);

 finally:

  Py_XDECREF(self->rollbackhook);
  self->rollbackhook=callable;

  Py_RETURN_NONE;
}

#ifdef EXPERIMENTAL /* sqlite3_profile */
static void
profilecb(void *context, const char *statement, sqlite_uint64 runtime)
{
  /* The hook returns void. That makes it impossible for us to
     abort immediately due to an error in the callback */
  
  PyGILState_STATE gilstate;
  PyObject *retval=NULL;
  Connection *self=(Connection *)context;

  assert(self);
  assert(self->profile);
  assert(self->profile!=Py_None);

  gilstate=PyGILState_Ensure();

  if(PyErr_Occurred())
    goto finally;  /* abort hook due to outstanding exception */

  retval=PyObject_CallFunction(self->profile, "(O&K)", convertutf8string, statement, runtime);

 finally:
  Py_XDECREF(retval);
  PyGILState_Release(gilstate);
}

static PyObject *
Connection_setprofile(Connection *self, PyObject *callable)
{
  /* sqlite3_profile doesn't return an error code */
  
  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  if(callable==Py_None)
    {
      sqlite3_profile(self->db, NULL, NULL);
      callable=NULL;
      goto finally;
    }

  if(!PyCallable_Check(callable))
    {
      PyErr_Format(PyExc_TypeError, "profile function must be callable");
      return NULL;
    }

  sqlite3_profile(self->db, profilecb, self);

  Py_INCREF(callable);

 finally:

  Py_XDECREF(self->profile);
  self->profile=callable;

  Py_RETURN_NONE;
}
#endif /* EXPERIMENTAL - sqlite3_profile */


#ifdef EXPERIMENTAL      /* commit hook */
static int 
commithookcb(void *context)
{
  /* The hook returns 0 for commit to go ahead and non-zero to abort
     commit (turn into a rollback). We return non-zero for errors */
  
  PyGILState_STATE gilstate;
  PyObject *retval=NULL;
  int ok=1; /* error state */
  Connection *self=(Connection *)context;

  assert(self);
  assert(self->commithook);
  assert(self->commithook!=Py_None);

  gilstate=PyGILState_Ensure();

  APSW_FAULT_INJECT(CommitHookExistingError,,PyErr_NoMemory());

  if(PyErr_Occurred())
    goto finally;  /* abort hook due to outstanding exception */

  retval=PyEval_CallObject(self->commithook, NULL);

  if(!retval)
    goto finally; /* abort hook due to exeception */

  ok=PyObject_IsTrue(retval);
  assert(ok==-1 || ok==0 || ok==1);
  if(ok==-1)
    {
      ok=1;
      goto finally;  /* abort due to exception in return value */
    }

 finally:
  Py_XDECREF(retval);
  PyGILState_Release(gilstate);
  return ok;
}

static PyObject *
Connection_setcommithook(Connection *self, PyObject *callable)
{
  /* sqlite3_commit_hook doesn't return an error code */
  
  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  if(callable==Py_None)
    {
      sqlite3_commit_hook(self->db, NULL, NULL);
      callable=NULL;
      goto finally;
    }

  if(!PyCallable_Check(callable))
    {
      PyErr_Format(PyExc_TypeError, "commit hook must be callable");
      return NULL;
    }

  sqlite3_commit_hook(self->db, commithookcb, self);

  Py_INCREF(callable);

 finally:

  Py_XDECREF(self->commithook);
  self->commithook=callable;

  Py_RETURN_NONE;
}
#endif  /* EXPERIMENTAL sqlite3_commit_hook */

#ifdef EXPERIMENTAL      /* sqlite3_progress_handler */
static int 
progresshandlercb(void *context)
{
  /* The hook returns 0 for continue and non-zero to abort (rollback).
     We return non-zero for errors */
  
  PyGILState_STATE gilstate;
  PyObject *retval=NULL;
  int ok=1; /* error state */
  Connection *self=(Connection *)context;

  assert(self);
  assert(self->progresshandler);

  gilstate=PyGILState_Ensure();

  retval=PyEval_CallObject(self->progresshandler, NULL);

  if(!retval)
    goto finally; /* abort due to exeception */

  ok=PyObject_IsTrue(retval);

  assert(ok==-1 || ok==0 || ok==1);
  if(ok==-1)
    {
      ok=1;
      goto finally;  /* abort due to exception in result */
    }

 finally:
  Py_XDECREF(retval);

  PyGILState_Release(gilstate);
  return ok;
}

static PyObject *
Connection_setprogresshandler(Connection *self, PyObject *args)
{
  /* sqlite3_progress_handler doesn't return an error code */
  int nsteps=20;
  PyObject *callable=NULL;
  
  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  if(!PyArg_ParseTuple(args, "O|i:setprogresshandler(callable, nsteps=20)", &callable, &nsteps))
    return NULL;

  if(callable==Py_None)
    {
      sqlite3_progress_handler(self->db, 0, NULL, NULL);
      callable=NULL;
      goto finally;
    }

  if(!PyCallable_Check(callable))
    {
      PyErr_Format(PyExc_TypeError, "progress handler must be callable");
      return NULL;
    }

  sqlite3_progress_handler(self->db, nsteps, progresshandlercb, self);
  Py_INCREF(callable);

 finally:

  Py_XDECREF(self->progresshandler);
  self->progresshandler=callable;

  Py_RETURN_NONE;
}
#endif  /* EXPERIMENTAL sqlite3_progress_handler */

static int 
authorizercb(void *context, int operation, const char *paramone, const char *paramtwo, const char *databasename, const char *triggerview)
{
  /* should return one of SQLITE_OK, SQLITE_DENY, or
     SQLITE_IGNORE. (0, 1 or 2 respectively) */

  PyGILState_STATE gilstate;
  PyObject *retval=NULL;
  int result=SQLITE_DENY;  /* default to deny */
  Connection *self=(Connection *)context;

  assert(self);
  assert(self->authorizer);
  assert(self->authorizer!=Py_None);

  gilstate=PyGILState_Ensure();

  APSW_FAULT_INJECT(AuthorizerExistingError,,PyErr_NoMemory());

  if(PyErr_Occurred())
    goto finally;  /* abort due to earlier exception */

  retval=PyObject_CallFunction(self->authorizer, "(iO&O&O&O&)", operation, convertutf8string, paramone, 
                               convertutf8string, paramtwo, convertutf8string, databasename, 
                               convertutf8string, triggerview);

  if(!retval)
    goto finally; /* abort due to exeception */

  if (PyIntLong_Check(retval))
    {
      result=PyIntLong_AsLong(retval);
      goto haveval;
    }
  
  PyErr_Format(PyExc_TypeError, "Authorizer must return a number");
  AddTraceBackHere(__FILE__, __LINE__, "authorizer callback", "{s: i, s: s:, s: s, s: s}",
                   "operation", operation, "paramone", paramone, "paramtwo", paramtwo, 
                   "databasename", databasename, "triggerview", triggerview);

 haveval:
  if (PyErr_Occurred())
    result=SQLITE_DENY;

 finally:
  Py_XDECREF(retval);

  PyGILState_Release(gilstate);
  return result;
}

static PyObject *
Connection_setauthorizer(Connection *self, PyObject *callable)
{
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  if(callable==Py_None)
    {
      APSW_FAULT_INJECT(SetAuthorizerNullFail,res=sqlite3_set_authorizer(self->db, NULL, NULL),res=SQLITE_IOERR);
      if(res!=SQLITE_OK)
        {
          SET_EXC(res, self->db);
          return NULL;
        }
      callable=NULL;
      goto finally;
    }

  if(!PyCallable_Check(callable))
    {
      PyErr_Format(PyExc_TypeError, "authorizer must be callable");
      return NULL;
    }

  APSW_FAULT_INJECT(SetAuthorizerFail,res=sqlite3_set_authorizer(self->db, authorizercb, self),res=SQLITE_IOERR);
  if(res!=SQLITE_OK)
    {
      SET_EXC(res, self->db);
      return NULL;
    }

  Py_INCREF(callable);

 finally:
  Py_XDECREF(self->authorizer);
  self->authorizer=callable;

  Py_RETURN_NONE;
}

static void
collationneeded_cb(void *pAux, APSW_ARGUNUSED sqlite3 *db, int eTextRep, const char *name)
{
  PyObject *res=NULL, *pyname=NULL;
  Connection *self=(Connection*)pAux;
  PyGILState_STATE gilstate=PyGILState_Ensure();

  assert(self->collationneeded);
  if(!self->collationneeded) goto finally;
  if(PyErr_Occurred()) goto finally;
  pyname=convertutf8string(name);
  if(pyname)  res=PyEval_CallFunction(self->collationneeded, "(OO)", self, pyname);
  if(!pyname || !res)
    AddTraceBackHere(__FILE__, __LINE__, "collationneeded callback", "{s: O, s: i, s: s}",
                     "Connection", self, "eTextRep", eTextRep, "name", name);
  Py_XDECREF(res);

 finally:
  Py_XDECREF(pyname);
  PyGILState_Release(gilstate);
}

static PyObject *
Connection_collationneeded(Connection *self, PyObject *callable)
{
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  if(callable==Py_None)
    {
      APSW_FAULT_INJECT(CollationNeededNullFail,res=sqlite3_collation_needed(self->db, NULL, NULL),res=SQLITE_IOERR);
      if(res!=SQLITE_OK)
        {
          SET_EXC(res, self->db);
          return NULL;
        }
      callable=NULL;
      goto finally;
    }

  if(!PyCallable_Check(callable))
    {
      PyErr_Format(PyExc_TypeError, "collationneeded callback must be callable");
      return NULL;
    }

  APSW_FAULT_INJECT(CollationNeededFail,res=sqlite3_collation_needed(self->db, self, collationneeded_cb), res=SQLITE_IOERR);
  if(res!=SQLITE_OK)
    {
      SET_EXC(res, self->db);
      return NULL;
    }

  Py_INCREF(callable);

 finally:
  Py_XDECREF(self->collationneeded);
  self->collationneeded=callable;

  Py_RETURN_NONE;
}

static int 
busyhandlercb(void *context, int ncall)
{
  /* Return zero for caller to get SQLITE_BUSY error. We default to
     zero in case of error. */

  PyGILState_STATE gilstate;
  PyObject *retval;
  int result=0;  /* default to fail with SQLITE_BUSY */
  Connection *self=(Connection *)context;

  assert(self);
  assert(self->busyhandler);

  gilstate=PyGILState_Ensure();

  retval=PyObject_CallFunction(self->busyhandler, "i", ncall);

  if(!retval)
    goto finally; /* abort due to exeception */

  result=PyObject_IsTrue(retval);
  assert(result==-1 || result==0 || result==1);
  Py_DECREF(retval);

  if(result==-1)
    {
      result=0;
      goto finally;  /* abort due to exception converting retval */
    }

 finally:
  PyGILState_Release(gilstate);
  return result;
}

#if defined(EXPERIMENTAL) && !defined(SQLITE_OMIT_LOAD_EXTENSION)  /* extension loading */
static PyObject *
Connection_enableloadextension(Connection *self, PyObject *enabled)
{
  int enabledp, res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  /* get the boolean value */
  enabledp=PyObject_IsTrue(enabled);
  if(enabledp==-1) return NULL;
  if (PyErr_Occurred()) return NULL;

  /* call function */
  APSW_FAULT_INJECT(EnableLoadExtensionFail, res=sqlite3_enable_load_extension(self->db, enabledp), res=SQLITE_IOERR);
  SET_EXC(res, self->db);

  /* done */
  if (res==SQLITE_OK)
    Py_RETURN_NONE;
  return NULL;
}

static PyObject *
Connection_loadextension(Connection *self, PyObject *args)
{
  int res;
  char *zfile=NULL, *zproc=NULL, *errmsg=NULL;

  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);
  
  if(!PyArg_ParseTuple(args, "es|z:loadextension(filename, entrypoint=None)", STRENCODING, &zfile, &zproc))
    return NULL;

  APSW_BEGIN_ALLOW_THREADS
    res=sqlite3_load_extension(self->db, zfile, zproc, &errmsg);
  APSW_END_ALLOW_THREADS;
  PyMem_Free(zfile);

  /* load_extension doesn't set the error message on the db so we have to make exception manually */
  if(res!=SQLITE_OK)
    {
      assert(errmsg);
      PyErr_Format(ExcExtensionLoading, "ExtensionLoadingError: %s", errmsg?errmsg:"unspecified");
      sqlite3_free(errmsg);
      return NULL;
    }
  Py_RETURN_NONE;
}

#endif /* EXPERIMENTAL extension loading */

static PyObject *
Connection_setbusyhandler(Connection *self, PyObject *callable)
{
  int res=SQLITE_OK;

  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  if(callable==Py_None)
    {
      APSW_FAULT_INJECT(SetBusyHandlerNullFail,res=sqlite3_busy_handler(self->db, NULL, NULL), res=SQLITE_IOERR);
      if(res!=SQLITE_OK)
        {
          SET_EXC(res, self->db);
          return NULL;
        }
      callable=NULL;
      goto finally;
    }

  if(!PyCallable_Check(callable))
    {
      PyErr_Format(PyExc_TypeError, "busyhandler must be callable");
      return NULL;
    }

  APSW_FAULT_INJECT(SetBusyHandlerFail,res=sqlite3_busy_handler(self->db, busyhandlercb, self), res=SQLITE_IOERR);
  if(res!=SQLITE_OK)
    {
      SET_EXC(res, self->db);
      return NULL;
    }

  Py_INCREF(callable);

 finally:
  Py_XDECREF(self->busyhandler);
  self->busyhandler=callable;

  Py_RETURN_NONE;
}


/* USER DEFINED FUNCTION CODE.*/

/* We store the registered functions in a linked list hooked into the
   connection object so we can free them.  There is probably a better
   data structure to use but this was most convenient. */

static funccbinfo *
freefunccbinfo(funccbinfo *func)
{
  funccbinfo *fnext;
  if(!func) 
    return NULL;

  if(func->name)
    PyMem_Free(func->name);
  Py_XDECREF(func->scalarfunc);
  Py_XDECREF(func->aggregatefactory);
  fnext=func->next;
  PyMem_Free(func);
  return fnext;
}

static funccbinfo *
allocfunccbinfo(void)
{
  funccbinfo *res=PyMem_Malloc(sizeof(funccbinfo));
  if(res)
    memset(res, 0, sizeof(funccbinfo));
  return res;
}


/* converts a python object into a sqlite3_context result */
static void
set_context_result(sqlite3_context *context, PyObject *obj)
{
  if(!obj)
    {
      assert(PyErr_Occurred());
      sqlite3_result_error_code(context, MakeSqliteMsgFromPyException(NULL));
      sqlite3_result_error(context, "bad object given to set_context_result", -1);
      return;
    }

  /* DUPLICATE(ish) code: this is substantially similar to the code in
     APSWCursor_dobinding.  If you fix anything here then do it there as
     well. */

  if(obj==Py_None)
    {
      sqlite3_result_null(context);
      return;
    }
#if PY_MAJOR_VERSION < 3
  if(PyInt_Check(obj))
    {
      sqlite3_result_int64(context, PyInt_AS_LONG(obj));
      return;
    }
#endif
  if (PyLong_Check(obj))
    {
      sqlite3_result_int64(context, PyLong_AsLongLong(obj));
      return;
    }
  if (PyFloat_Check(obj))
    {
      sqlite3_result_double(context, PyFloat_AS_DOUBLE(obj));
      return;
    }
  if (PyUnicode_Check(obj))
    {
      UNIDATABEGIN(obj)
        APSW_FAULT_INJECT(SetContextResultUnicodeConversionFails,,strdata=(char*)PyErr_NoMemory());
        if(strdata)
          {
#ifdef APSW_TEST_LARGE_OBJECTS
            APSW_FAULT_INJECT(SetContextResultLargeUnicode,,strbytes=0x001234567890L);
#endif
	    if(strbytes>APSW_INT32_MAX)
	      {
                SET_EXC(SQLITE_TOOBIG, NULL);
                sqlite3_result_error_toobig(context);
	      }
	    else
              USE16(sqlite3_result_text)(context, strdata, strbytes, SQLITE_TRANSIENT);
          }
        else
          sqlite3_result_error(context, "Unicode conversions failed", -1);
      UNIDATAEND(obj);
      return;
    }
#if PY_MAJOR_VERSION < 3
  if (PyString_Check(obj))
    {
      const char *val=PyString_AS_STRING(obj);
      const Py_ssize_t lenval=PyString_GET_SIZE(obj);
      const char *chk=val;
      /* check if string is all ascii if less than 10kb in size */
      if(lenval<10000)
        for(;chk<val+lenval && !((*chk)&0x80); chk++);
      /* Non-ascii or long, so convert to unicode */
      if(chk<val+lenval)
        {
          PyObject *str2=PyUnicode_FromObject(obj);
          if(!str2)
            {
              sqlite3_result_error(context, "PyUnicode_FromObject failed", -1);
              return;
            }
          UNIDATABEGIN(str2)
            APSW_FAULT_INJECT(SetContextResultStringUnicodeConversionFails,,strdata=(char*)PyErr_NoMemory());
            if(strdata)
              {
#ifdef APSW_TEST_LARGE_OBJECTS
                APSW_FAULT_INJECT(SetContextResultLargeString,,strbytes=0x001234567890L);
#endif
		if(strbytes>APSW_INT32_MAX)
		  {
                    SET_EXC(SQLITE_TOOBIG, NULL);
                    sqlite3_result_error_toobig(context);
		  }
		else
                  USE16(sqlite3_result_text)(context, strdata, strbytes, SQLITE_TRANSIENT);
              }
            else
              sqlite3_result_error(context, "Unicode conversions failed", -1);
          UNIDATAEND(str2);
          Py_DECREF(str2);
        }
      else /* just ascii chars */
        sqlite3_result_text(context, val, lenval, SQLITE_TRANSIENT);

      return;
    }
#endif
  if (PyObject_CheckReadBuffer(obj))
    {
      const void *buffer;
      Py_ssize_t buflen;
      int asrb=PyObject_AsReadBuffer(obj, &buffer, &buflen);

      APSW_FAULT_INJECT(SetContextResultAsReadBufferFail,,(PyErr_NoMemory(),asrb=-1));

      if(asrb!=0)
        {
          sqlite3_result_error(context, "PyObject_AsReadBuffer failed", -1);
          return;
        }
      if (buflen>APSW_INT32_MAX)
	sqlite3_result_error_toobig(context);
      else
	sqlite3_result_blob(context, buffer, buflen, SQLITE_TRANSIENT);
      return;
    }

  PyErr_Format(PyExc_TypeError, "Bad return type from function callback");
  sqlite3_result_error(context, "Bad return type from function callback", -1);
}

/* Returns a new reference to a tuple formed from function parameters */
static PyObject *
getfunctionargs(sqlite3_context *context, PyObject *firstelement, int argc, sqlite3_value **argv)
{
  PyObject *pyargs=NULL;
  int i;
  int extra=0;

  /* extra first item */
  if(firstelement)
    extra=1;

  APSW_FAULT_INJECT(GFAPyTuple_NewFail,pyargs=PyTuple_New((long)argc+extra),pyargs=PyErr_NoMemory());
  if(!pyargs)
    {
      sqlite3_result_error(context, "PyTuple_New failed", -1);
      goto error;
    }

  if(extra)
    {
      Py_INCREF(firstelement);
      PyTuple_SET_ITEM(pyargs, 0, firstelement);
    }

  for(i=0;i<argc;i++)
    {
      PyObject *item=convert_value_to_pyobject(argv[i]);
      if(!item)
        {
          sqlite3_result_error(context, "convert_value_to_pyobject failed", -1);
          goto error;
        }
      PyTuple_SET_ITEM(pyargs, i+extra, item);
    }
  
  return pyargs;

 error:
  Py_XDECREF(pyargs);
  return NULL;
}


/* dispatches scalar function */
static void
cbdispatch_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
  PyGILState_STATE gilstate;
  PyObject *pyargs=NULL;
  PyObject *retval=NULL;
  funccbinfo *cbinfo=(funccbinfo*)sqlite3_user_data(context);
  assert(cbinfo);

  gilstate=PyGILState_Ensure();

  assert(cbinfo->scalarfunc);


  APSW_FAULT_INJECT(CBDispatchExistingError,,PyErr_NoMemory());

  if(PyErr_Occurred())
    {
      sqlite3_result_error_code(context, MakeSqliteMsgFromPyException(NULL));
      sqlite3_result_error(context, "Prior Python Error", -1);
      goto finalfinally;
    }

  pyargs=getfunctionargs(context, NULL, argc, argv);
  if(!pyargs)
      goto finally;

  assert(!PyErr_Occurred());
  retval=PyEval_CallObject(cbinfo->scalarfunc, pyargs);
  if(retval)
    set_context_result(context, retval);

 finally:
  if (PyErr_Occurred())
    {
      char *errmsg=NULL;
      char *funname=sqlite3_mprintf("user-defined-scalar-%s", cbinfo->name);
      sqlite3_result_error_code(context, MakeSqliteMsgFromPyException(&errmsg));
      sqlite3_result_error(context, errmsg, -1);
      AddTraceBackHere(__FILE__, __LINE__, funname, "{s: i, s: s}", "NumberOfArguments", argc, "message", errmsg);
      sqlite3_free(funname);
      sqlite3_free(errmsg);
    }
 finalfinally:
  Py_XDECREF(pyargs);
  Py_XDECREF(retval);
  
  PyGILState_Release(gilstate);
}

static aggregatefunctioncontext *
getaggregatefunctioncontext(sqlite3_context *context)
{
  aggregatefunctioncontext *aggfc=sqlite3_aggregate_context(context, sizeof(aggregatefunctioncontext));
  funccbinfo *cbinfo;
  PyObject *retval;
  /* have we seen it before? */
  if(aggfc->aggvalue) 
    return aggfc;
  
  /* fill in with Py_None so we know it is valid */
  aggfc->aggvalue=Py_None;
  Py_INCREF(Py_None);

  cbinfo=(funccbinfo*)sqlite3_user_data(context);
  assert(cbinfo);
  assert(cbinfo->aggregatefactory);

  /* call the aggregatefactory to get our working objects */
  retval=PyEval_CallObject(cbinfo->aggregatefactory, NULL);

  if(!retval)
    return aggfc;
  /* it should have returned a tuple of 3 items: object, stepfunction and finalfunction */
  if(!PyTuple_Check(retval))
    {
      PyErr_Format(PyExc_TypeError, "Aggregate factory should return tuple of (object, stepfunction, finalfunction)");
      goto finally;
    }
  if(PyTuple_GET_SIZE(retval)!=3)
    {
      PyErr_Format(PyExc_TypeError, "Aggregate factory should return 3 item tuple of (object, stepfunction, finalfunction)");
      goto finally;
    }
  /* we don't care about the type of the zeroth item (object) ... */

  /* stepfunc */
  if (!PyCallable_Check(PyTuple_GET_ITEM(retval,1)))
    {
      PyErr_Format(PyExc_TypeError, "stepfunction must be callable");
      goto finally;
    }
  
  /* finalfunc */
  if (!PyCallable_Check(PyTuple_GET_ITEM(retval,2)))
    {
      PyErr_Format(PyExc_TypeError, "final function must be callable");
      goto finally;
    }

  aggfc->aggvalue=PyTuple_GET_ITEM(retval,0);
  aggfc->stepfunc=PyTuple_GET_ITEM(retval,1);
  aggfc->finalfunc=PyTuple_GET_ITEM(retval,2);

  Py_INCREF(aggfc->aggvalue);
  Py_INCREF(aggfc->stepfunc);
  Py_INCREF(aggfc->finalfunc);
      
  Py_DECREF(Py_None);  /* we used this earlier as a sentinel */

 finally:
  assert(retval);
  Py_DECREF(retval);
  return aggfc;
}


/*
  Note that we can't call sqlite3_result_error in the step function as
  SQLite doesn't want to you to do that (and core dumps!)
  Consequently if an error is returned, we will still be repeatedly
  called.
*/

static void
cbdispatch_step(sqlite3_context *context, int argc, sqlite3_value **argv)
{
  PyGILState_STATE gilstate;
  PyObject *pyargs;
  PyObject *retval;
  aggregatefunctioncontext *aggfc=NULL;

  gilstate=PyGILState_Ensure();

  if (PyErr_Occurred())
    goto finalfinally;

  aggfc=getaggregatefunctioncontext(context);

  if (PyErr_Occurred())
    goto finally;

  assert(aggfc);
  
  pyargs=getfunctionargs(context, aggfc->aggvalue, argc, argv);
  if(!pyargs)
    goto finally;

  assert(!PyErr_Occurred());
  retval=PyEval_CallObject(aggfc->stepfunc, pyargs);
  Py_DECREF(pyargs);
  Py_XDECREF(retval);

  if(!retval)
    {
      assert(PyErr_Occurred());
    }

 finally:
  if(PyErr_Occurred())
    {
      char *funname=0;
      funccbinfo *cbinfo=(funccbinfo*)sqlite3_user_data(context);
      assert(cbinfo);
      funname=sqlite3_mprintf("user-defined-aggregate-step-%s", cbinfo->name);
      AddTraceBackHere(__FILE__, __LINE__, funname, "{s: i}", "NumberOfArguments", argc);
      sqlite3_free(funname);
    }
 finalfinally:
  PyGILState_Release(gilstate);
}

/* this is somewhat similar to cbdispatch_step, except we also have to
   do some cleanup of the aggregatefunctioncontext */
static void
cbdispatch_final(sqlite3_context *context)
{
  PyGILState_STATE gilstate;
  PyObject *retval=NULL;
  aggregatefunctioncontext *aggfc=NULL;
  PyObject *err_type=NULL, *err_value=NULL, *err_traceback=NULL;

  gilstate=PyGILState_Ensure();

  PyErr_Fetch(&err_type, &err_value, &err_traceback);

  aggfc=getaggregatefunctioncontext(context);
  assert(aggfc);

  APSW_FAULT_INJECT(CBDispatchFinalError,,PyErr_NoMemory());
  
  if((err_type||err_value||err_traceback) || PyErr_Occurred() || !aggfc->finalfunc)
    {
      sqlite3_result_error(context, "Prior Python Error in step function", -1);
      goto finally;
    }

  retval=PyObject_CallFunctionObjArgs(aggfc->finalfunc, aggfc->aggvalue, NULL);
  set_context_result(context, retval);
  Py_XDECREF(retval);

 finally:
  /* we also free the aggregatefunctioncontext here */
  assert(aggfc->aggvalue);  /* should always be set, perhaps to Py_None */
  Py_XDECREF(aggfc->aggvalue);
  Py_XDECREF(aggfc->stepfunc);
  Py_XDECREF(aggfc->finalfunc);

  if(PyErr_Occurred() && (err_type||err_value||err_traceback))
    {
      PyErr_Format(PyExc_Exception, "An exception happened during cleanup of an aggregate function, but there was already error in the step function so only that can be returned");
      apsw_write_unraiseable(NULL);
    }

  if(err_type||err_value||err_traceback)
    PyErr_Restore(err_type, err_value, err_traceback);

  if(PyErr_Occurred())
    {
      char *funname=0;
      funccbinfo *cbinfo=(funccbinfo*)sqlite3_user_data(context);
      assert(cbinfo);
      funname=sqlite3_mprintf("user-defined-aggregate-final-%s", cbinfo->name);
      AddTraceBackHere(__FILE__, __LINE__, funname, NULL);
      sqlite3_free(funname);
    }

  /* sqlite3 frees the actual underlying memory we used (aggfc itself) */

  PyGILState_Release(gilstate);
}


static PyObject *
Connection_createscalarfunction(Connection *self, PyObject *args)
{
  int numargs=-1;
  PyObject *callable;
  char *name=0;
  char *chk;
  funccbinfo *cbinfo;
  int res;
 
  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  if(!PyArg_ParseTuple(args, "esO|i:createscalarfunction(name,callback, numargs=-1)", STRENCODING, &name, &callable, &numargs))
    return NULL;

  assert(name);
  assert(callable);

  /* there isn't a C api to get a (potentially unicode) string and
     make it uppercase so we hack around  */

  /* validate the name */
  for(chk=name;*chk && !((*chk)&0x80);chk++);
  if(*chk)
    {
      PyMem_Free(name);
      PyErr_SetString(PyExc_TypeError, "function name must be ascii characters only");
      return NULL;
    }

  /* convert name to upper case */
  for(chk=name;*chk;chk++)
    if(*chk>='a' && *chk<='z')
      *chk-='a'-'A';

  /* ::TODO:: check if name points to already defined function and free relevant funccbinfo */

  if(callable!=Py_None && !PyCallable_Check(callable))
    {
      PyMem_Free(name);
      PyErr_SetString(PyExc_TypeError, "parameter must be callable");
      return NULL;
    }

  Py_INCREF(callable);

  cbinfo=allocfunccbinfo();
  cbinfo->name=name;
  cbinfo->scalarfunc=callable;

  res=sqlite3_create_function(self->db,
                              name,
                              numargs,
                              SQLITE_UTF8,  /* it isn't very clear what this parameter does */
                              (callable!=Py_None)?cbinfo:NULL,
                              (callable!=Py_None)?cbdispatch_func:NULL,
                              NULL,
                              NULL);

  if(res)
    {
      freefunccbinfo(cbinfo);
      SET_EXC(res, self->db);
      return NULL;
    }

  if(callable!=Py_None)
    {
      /* put cbinfo into the linked list */
      cbinfo->next=self->functions;
      self->functions=cbinfo;
    }
  else
    {
      /* free it since we cancelled the function */
      freefunccbinfo(cbinfo);
    }
  
  Py_RETURN_NONE;
}

static PyObject *
Connection_createaggregatefunction(Connection *self, PyObject *args)
{
  int numargs=-1;
  PyObject *callable;
  char *name=0;
  char *chk;
  funccbinfo *cbinfo;
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  if(!PyArg_ParseTuple(args, "esO|i:createaggregatefunction(name, factorycallback, numargs=-1)", STRENCODING, &name, &callable, &numargs))
    return NULL;

  assert(name);
  assert(callable);

  /* there isn't a C api to get a (potentially unicode) string and make it uppercase so we hack around  */

  /* validate the name */
  for(chk=name;*chk && !((*chk)&0x80);chk++);
  if(*chk)
    {
      PyMem_Free(name);
      PyErr_SetString(PyExc_TypeError, "function name must be ascii characters only");
      return NULL;
    }

  /* convert name to upper case */
  for(chk=name;*chk;chk++)
    if(*chk>='a' && *chk<='z')
      *chk-='a'-'A';

  /* ::TODO:: check if name points to already defined function and free relevant funccbinfo */

  if(callable!=Py_None && !PyCallable_Check(callable))
    {
      PyMem_Free(name);
      PyErr_SetString(PyExc_TypeError, "parameter must be callable");
      return NULL;
    }

  Py_INCREF(callable);

  cbinfo=allocfunccbinfo();
  cbinfo->name=name;
  cbinfo->aggregatefactory=callable;

  res=sqlite3_create_function(self->db,
                              name,
                              numargs,
                              SQLITE_UTF8,  /* it isn't very clear what this parameter does */
                              (callable!=Py_None)?cbinfo:NULL,
                              NULL,
                              (callable!=Py_None)?cbdispatch_step:NULL,
                              (callable!=Py_None)?cbdispatch_final:NULL);

  if(res)
    {
      freefunccbinfo(cbinfo);
      SET_EXC(res, self->db);
      return NULL;
    }

  if(callable!=Py_None)
    {
      /* put cbinfo into the linked list */
      cbinfo->next=self->functions;
      self->functions=cbinfo;
    }
  else
    {
      /* free things up */
      freefunccbinfo(cbinfo);
    }
  
  Py_RETURN_NONE;
}

/* USER DEFINED COLLATION CODE.*/

static int 
collation_cb(void *context, 
	     int stringonelen, const void *stringonedata,
	     int stringtwolen, const void *stringtwodata)
{
  PyGILState_STATE gilstate;
  PyObject *cbinfo=(PyObject*)context;
  PyObject *pys1=NULL, *pys2=NULL, *retval=NULL;
  int result=0;

  assert(cbinfo);

  gilstate=PyGILState_Ensure();

  if(PyErr_Occurred()) goto finally;  /* outstanding error */

  pys1=convertutf8stringsize(stringonedata, stringonelen);
  pys2=convertutf8stringsize(stringtwodata, stringtwolen);

  if(!pys1 || !pys2)  
    goto finally;   /* failed to allocate strings */

  retval=PyObject_CallFunction(cbinfo, "(OO)", pys1, pys2);

  if(!retval) 
    {
      AddTraceBackHere(__FILE__, __LINE__, "Collation_callback", "{s: O, s: O, s: O}", "callback", cbinfo, "stringone", pys1, "stringtwo", pys2);
      goto finally;  /* execution failed */
    }

  if (PyIntLong_Check(retval))
    {
      result=PyIntLong_AsLong(retval);
      goto haveval;
    }
  
  PyErr_Format(PyExc_TypeError, "Collation callback must return a number");
  AddTraceBackHere(__FILE__, __LINE__, "collation callback", "{s: O, s: O}",
                   "stringone", pys1, "stringtwo", pys2);

 haveval:
  if(PyErr_Occurred())
      result=0;

 finally:
  Py_XDECREF(pys1);
  Py_XDECREF(pys2);
  Py_XDECREF(retval);
  PyGILState_Release(gilstate);
  return result;

}

static void
collation_destroy(void *context)
{
  PyGILState_STATE gilstate=PyGILState_Ensure();
  Py_DECREF((PyObject*)context);
  PyGILState_Release(gilstate);
}

static PyObject *
Connection_createcollation(Connection *self, PyObject *args)
{
  PyObject *callable=NULL;
  char *name=0;
  int res;

  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);
  
  if(!PyArg_ParseTuple(args, "esO:createcollation(name,callback)", STRENCODING, &name, &callable))
      return NULL;

  assert(name);
  assert(callable);

  if(callable!=Py_None && !PyCallable_Check(callable))
    {
      PyMem_Free(name);
      PyErr_SetString(PyExc_TypeError, "parameter must be callable");
      return NULL;
    }

  res=sqlite3_create_collation_v2(self->db,
                                  name,
                                  SQLITE_UTF8,
                                  (callable!=Py_None)?callable:NULL,
                                  (callable!=Py_None)?collation_cb:NULL,
                                  (callable!=Py_None)?collation_destroy:NULL);
  PyMem_Free(name);
  if(res!=SQLITE_OK)
    {
      SET_EXC(res, self->db);
      return NULL;
    }

  if (callable!=Py_None)
    Py_INCREF(callable);
  
  Py_RETURN_NONE;
}

static PyObject *
Connection_filecontrol(Connection *self, PyObject *args)
{
  PyObject *pyptr;
  void *ptr=NULL;
  int res, op;
  char *dbname=NULL;

  CHECK_USE(NULL);
  CHECK_CLOSED(self,NULL);

  if(!PyArg_ParseTuple(args, "esiO", STRENCODING, &dbname, &op, &pyptr))
    return NULL;

  if(PyIntLong_Check(pyptr))
    ptr=PyLong_AsVoidPtr(pyptr);
  else
    PyErr_Format(PyExc_TypeError, "Argument is not a number (pointer)");

  if(PyErr_Occurred())
    {
      AddTraceBackHere(__FILE__, __LINE__, "Connection.filecontrol", "{s: O}", "args", args);
      goto finally;
    }

  res=sqlite3_file_control(self->db, dbname, op, ptr);

  SET_EXC(res, self->db);

 finally:
  if(dbname) PyMem_Free(dbname);
  
  if(PyErr_Occurred())
    return NULL;

  Py_RETURN_NONE;
}

static PyObject*
Connection_sqlite3pointer(Connection *self)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self, NULL);

  return PyLong_FromVoidPtr(self->db);
}


static PyMethodDef Connection_methods[] = {
  {"cursor", (PyCFunction)Connection_cursor, METH_NOARGS,
   "Create a new cursor" },
  {"close",  (PyCFunction)Connection_close, METH_VARARGS,
   "Closes the connection" },
  {"setbusytimeout", (PyCFunction)Connection_setbusytimeout, METH_VARARGS,
   "Sets the sqlite busy timeout in milliseconds.  Use zero to disable the timeout"},
  {"interrupt", (PyCFunction)Connection_interrupt, METH_NOARGS,
   "Causes any pending database operations to abort at the earliest opportunity"},
  {"createscalarfunction", (PyCFunction)Connection_createscalarfunction, METH_VARARGS,
   "Creates a scalar function"},
  {"createaggregatefunction", (PyCFunction)Connection_createaggregatefunction, METH_VARARGS,
   "Creates an aggregate function"},
  {"setbusyhandler", (PyCFunction)Connection_setbusyhandler, METH_O,
   "Sets the busy handler"},
  {"changes", (PyCFunction)Connection_changes, METH_NOARGS, 
   "Returns the number of rows changed by last query"},
  {"totalchanges", (PyCFunction)Connection_totalchanges, METH_NOARGS, 
   "Returns the total number of changes to database since it was opened"},
  {"getautocommit", (PyCFunction)Connection_getautocommit, METH_NOARGS, 
   "Returns if the database is in auto-commit mode"},
  {"createcollation", (PyCFunction)Connection_createcollation, METH_VARARGS,
   "Creates a collation function"},
  {"last_insert_rowid", (PyCFunction)Connection_last_insert_rowid, METH_NOARGS,
   "Returns rowid for last insert"},
  {"complete", (PyCFunction)Connection_complete, METH_VARARGS,
   "Checks if a SQL statement is complete"},
  {"collationneeded", (PyCFunction)Connection_collationneeded, METH_O,
   "Sets collation needed callback"},
  {"setauthorizer", (PyCFunction)Connection_setauthorizer, METH_O,
   "Sets an authorizer function"},
  {"setupdatehook", (PyCFunction)Connection_setupdatehook, METH_O,
      "Sets an update hook"},
  {"setrollbackhook", (PyCFunction)Connection_setrollbackhook, METH_O,
   "Sets a callable invoked before each rollback"},
  {"blobopen", (PyCFunction)Connection_blobopen, METH_VARARGS,
   "Opens a blob for i/o"},
#ifdef EXPERIMENTAL
  {"limit", (PyCFunction)Connection_limit, METH_VARARGS,
   "Gets and sets limits"},
  {"setprofile", (PyCFunction)Connection_setprofile, METH_O,
   "Sets a callable invoked with profile information after each statement"},
  {"setcommithook", (PyCFunction)Connection_setcommithook, METH_O,
   "Sets a callable invoked before each commit"},
  {"setprogresshandler", (PyCFunction)Connection_setprogresshandler, METH_VARARGS,
   "Sets a callback invoked periodically during long running calls"},
#if !defined(SQLITE_OMIT_LOAD_EXTENSION)
  {"enableloadextension", (PyCFunction)Connection_enableloadextension, METH_O,
   "Enables loading of SQLite extensions from shared libraries"},
  {"loadextension", (PyCFunction)Connection_loadextension, METH_VARARGS,
   "loads SQLite extension"},
#endif
  {"createmodule", (PyCFunction)Connection_createmodule, METH_VARARGS,
   "registers a virtual table"},
#endif
  {"filecontrol", (PyCFunction)Connection_filecontrol, METH_VARARGS,
   "file control"},
  {"sqlite3pointer", (PyCFunction)Connection_sqlite3pointer, METH_NOARGS,
   "gets underlying pointer"},
  {0, 0, 0, 0}  /* Sentinel */
};


static PyTypeObject ConnectionType = 
  {
#if PY_MAJOR_VERSION < 3
    PyObject_HEAD_INIT(NULL)
    0,                         /*ob_size*/
#else
    PyVarObject_HEAD_INIT(NULL,0)
#endif
    "apsw.Connection",         /*tp_name*/
    sizeof(Connection),        /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)Connection_dealloc, /*tp_dealloc*/ 
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_VERSION_TAG, /*tp_flags*/
    "Connection object",       /* tp_doc */
    0,		               /* tp_traverse */
    0,		               /* tp_clear */
    0,		               /* tp_richcompare */
    0,		               /* tp_weaklistoffset */
    0,		               /* tp_iter */
    0,		               /* tp_iternext */
    Connection_methods,        /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Connection_init, /* tp_init */
    0,                         /* tp_alloc */
    Connection_new,            /* tp_new */
    0,                         /* tp_free */
    0,                         /* tp_is_gc */
    0,                         /* tp_bases */
    0,                         /* tp_mro */
    0,                         /* tp_cache */
    0,                         /* tp_subclasses */
    0,                         /* tp_weaklist */
    0,                         /* tp_del */
#if PY_VERSION_HEX>=0x02060000
    0                          /* tp_version_tag */
#endif
};




/* CURSOR CODE */

/* Do finalization and free resources.  Returns the SQLITE error code */
static int
resetcursor(APSWCursor *self, int force)
{
  int res=SQLITE_OK;
  PyObject *nextquery=self->statement?self->statement->next:NULL;
  PyObject *etype, *eval, *etb;

  if(force)
    PyErr_Fetch(&etype, &eval, &etb);

  if(nextquery) 
    Py_INCREF(nextquery);

  Py_XDECREF(self->bindings);
  self->bindings=NULL;
  self->bindingsoffset= -1;

  if(self->statement)
    {
      res=statementcache_finalize(self->connection->stmtcache, self->statement);
      if(!force) /* we don't care about errors when forcing */
	SET_EXC(res, self->connection->db);
      self->statement=0;
    }

  if(!force && self->status!=C_DONE && nextquery)
    {
      if (res==SQLITE_OK)
        {
          /* We still have more, so this is actually an abort. */
          res=SQLITE_ERROR;
          if(!PyErr_Occurred())
            {
              PyErr_Format(ExcIncomplete, "Error: there are still remaining sql statements to execute");
              AddTraceBackHere(__FILE__, __LINE__, "resetcursor", "{s: N}", "remaining", convertutf8buffertounicode(nextquery));
            }
        }
    }

  Py_XDECREF(nextquery);
  
  if(!force && self->status!=C_DONE && self->emiter)
    {
      PyObject *next=PyIter_Next(self->emiter);
      if(next)
        {
          Py_DECREF(next);
          res=SQLITE_ERROR;
          if (!PyErr_Occurred())
	    /* Technically this line won't get executed since the
	       block above will already have set ExcIncomplete.
	       Leaving it in as defensive coding. */
            PyErr_Format(ExcIncomplete, "Error: there are still many remaining sql statements to execute");
        }
    }
     
  Py_XDECREF(self->emiter);
  self->emiter=NULL;
  Py_XDECREF(self->emoriginalquery);
  self->emoriginalquery=NULL;

  self->status=C_DONE;

  if (PyErr_Occurred())
    {
      assert(res);
      AddTraceBackHere(__FILE__, __LINE__, "resetcursor", "{s: i}", "res", res);
    }

  if(force && (etype || eval || etb))
    PyErr_Restore(etype, eval, etb);

  return res;
}

static void
APSWCursor_dealloc(APSWCursor * self)
{
  PyObject *err_type, *err_value, *err_traceback;
  int have_error=PyErr_Occurred()?1:0;

  /* do our finalisation ... */

  if (have_error)
    {
      /* remember the existing error so that resetcursor won't immediately return */
      PyErr_Fetch(&err_type, &err_value, &err_traceback);
    }

  resetcursor(self, /* force = */ 1);
  assert(!PyErr_Occurred());

  if (have_error)
    /* restore earlier error if there was one */
    PyErr_Restore(err_type, err_value, err_traceback);

  /* we no longer need connection */
  if(self->connection)
    {
      pointerlist_remove(&self->connection->dependents, self);
      Py_DECREF(self->connection);
      self->connection=0;
    }

  /* executemany iterator */
  Py_XDECREF(self->emiter);
  self->emiter=NULL;

  /* no need for tracing */
  Py_XDECREF(self->exectrace);
  Py_XDECREF(self->rowtrace);
  self->exectrace=self->rowtrace=0;
  
  Py_TYPE(self)->tp_free((PyObject*)self);
}

static void
APSWCursor_init(APSWCursor *self, Connection *connection)
{
  self->connection=connection;
  self->statement=0;
  self->status=C_DONE;
  self->bindings=0;
  self->bindingsoffset=0;
  self->emiter=0;
  self->emoriginalquery=0;
  self->exectrace=0;
  self->rowtrace=0;
  self->inuse=0;
}

static PyObject *
APSWCursor_getdescription(APSWCursor *self)
{
  int ncols,i;
  PyObject *result=NULL;
  PyObject *pair=NULL;

  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection,NULL);

  if(!self->statement)
    {
      PyErr_Format(ExcComplete, "Can't get description for statements that have completed execution");
      return NULL;
    }
  
  ncols=sqlite3_column_count(self->statement->vdbestatement);
  result=PyTuple_New(ncols);
  if(!result) goto error;

  for(i=0;i<ncols;i++)
    {
      APSW_FAULT_INJECT(GetDescriptionFail,
      pair=Py_BuildValue("(O&O&)", 
			 convertutf8string, sqlite3_column_name(self->statement->vdbestatement, i),
			 convertutf8string, sqlite3_column_decltype(self->statement->vdbestatement, i)),
      pair=PyErr_NoMemory()
      );                  
			 
      if(!pair) goto error;

      PyTuple_SET_ITEM(result, i, pair);
      /* owned by result now */
      pair=0;
    }
  
  return result;

 error:
  Py_XDECREF(result);
  Py_XDECREF(pair);
  return NULL;
}

/* internal function - returns SQLite error code (ie SQLITE_OK if all is well) */
static int
APSWCursor_dobinding(APSWCursor *self, int arg, PyObject *obj)
{

  /* DUPLICATE(ish) code: this is substantially similar to the code in
     set_context_result.  If you fix anything here then do it there as
     well. */

  int res=SQLITE_OK;

  assert(!PyErr_Occurred());

  if(obj==Py_None)
    res=sqlite3_bind_null(self->statement->vdbestatement, arg);
  /* Python uses a 'long' for storage of PyInt.  This could
     be a 32bit or 64bit quantity depending on the platform. */
#if PY_MAJOR_VERSION < 3
  else if(PyInt_Check(obj))
    res=sqlite3_bind_int64(self->statement->vdbestatement, arg, PyInt_AS_LONG(obj));
#endif
  else if (PyLong_Check(obj))
    /* nb: PyLong_AsLongLong can cause Python level error */
    res=sqlite3_bind_int64(self->statement->vdbestatement, arg, PyLong_AsLongLong(obj));
  else if (PyFloat_Check(obj))
    res=sqlite3_bind_double(self->statement->vdbestatement, arg, PyFloat_AS_DOUBLE(obj));
  else if (PyUnicode_Check(obj))
    {
      const void *badptr=NULL;
      UNIDATABEGIN(obj)
        APSW_FAULT_INJECT(DoBindingUnicodeConversionFails,,strdata=(char*)PyErr_NoMemory());
        badptr=strdata;
#ifdef APSW_TEST_LARGE_OBJECTS
        APSW_FAULT_INJECT(DoBindingLargeUnicode,,strbytes=0x001234567890L);
#endif
        if(strdata)
          {
	    if(strbytes>APSW_INT32_MAX)
	      {
                SET_EXC(SQLITE_TOOBIG, NULL);
	      }
	    else
              res=USE16(sqlite3_bind_text)(self->statement->vdbestatement, arg, strdata, strbytes, SQLITE_TRANSIENT);
          }
      UNIDATAEND(obj);
      if(!badptr) 
        {
          assert(PyErr_Occurred());
          return -1;
        }
    }
#if PY_MAJOR_VERSION < 3
  else if (PyString_Check(obj))
    {
      const char *val=PyString_AS_STRING(obj);
      const size_t lenval=PyString_GET_SIZE(obj);
      const char *chk=val;

      if(lenval<10000)
        for(;chk<val+lenval && !((*chk)&0x80); chk++);
      if(chk<val+lenval)
        {
          const void *badptr=NULL;
          PyObject *str2=PyUnicode_FromObject(obj);
          if(!str2)
            return -1;
          UNIDATABEGIN(str2)
            APSW_FAULT_INJECT(DoBindingStringConversionFails,,strdata=(char*)PyErr_NoMemory());
#ifdef APSW_TEST_LARGE_OBJECTS
            APSW_FAULT_INJECT(DoBindingLargeString,,strbytes=0x001234567890L);
#endif
            badptr=strdata;
            if(strdata)
              {
		if(strbytes>APSW_INT32_MAX)
		  {
                    SET_EXC(SQLITE_TOOBIG, NULL);
                    res=SQLITE_TOOBIG;
		  }
		else
                  res=USE16(sqlite3_bind_text)(self->statement->vdbestatement, arg, strdata, strbytes, SQLITE_TRANSIENT);
              }
          UNIDATAEND(str2);
          Py_DECREF(str2);
          if(!badptr) 
            {
              assert(PyErr_Occurred());
              return -1;
            }
        }
      else
	{
	  assert(lenval<APSW_INT32_MAX);
	  res=sqlite3_bind_text(self->statement->vdbestatement, arg, val, lenval, SQLITE_TRANSIENT);
	}
    }
#endif
  else if (PyObject_CheckReadBuffer(obj))
    {
      const void *buffer;
      Py_ssize_t buflen;
      int asrb;
      
      APSW_FAULT_INJECT(DoBindingAsReadBufferFails,asrb=PyObject_AsReadBuffer(obj, &buffer, &buflen), (PyErr_NoMemory(), asrb=-1));
      if(asrb!=0)
        return -1;

      if (buflen>APSW_INT32_MAX)
	{
          SET_EXC(SQLITE_TOOBIG, NULL);
	  return -1;
	}
      res=sqlite3_bind_blob(self->statement->vdbestatement, arg, buffer, buflen, SQLITE_TRANSIENT);
    }
  else if(PyObject_TypeCheck(obj, &ZeroBlobBindType)==1)
    {
      res=sqlite3_bind_zeroblob(self->statement->vdbestatement, arg, ((ZeroBlobBind*)obj)->blobsize);
    }
  else 
    {
      PyErr_Format(PyExc_TypeError, "Bad binding argument type supplied - argument #%d: type %s", (int)(arg+self->bindingsoffset), Py_TYPE(obj)->tp_name);
      return -1;
    }
  if(res!=SQLITE_OK)
    {
      SET_EXC(res, self->connection->db);
      return -1;
    }
  if(PyErr_Occurred())
    return -1;
  return 0;
}

/* internal function */
static int
APSWCursor_dobindings(APSWCursor *self)
{
  int nargs, arg, res, sz=0;
  PyObject *obj;

  assert(!PyErr_Occurred());
  assert(self->bindingsoffset>=0);

  nargs=sqlite3_bind_parameter_count(self->statement->vdbestatement);
  if(nargs==0 && !self->bindings)
    return 0; /* common case, no bindings needed or supplied */

  if (nargs>0 && !self->bindings)
    {
      PyErr_Format(ExcBindings, "Statement has %d bindings but you didn't supply any!", nargs);
      return -1;
    }

  /* a dictionary? */
  if (self->bindings && PyDict_Check(self->bindings))
    {
      for(arg=1;arg<=nargs;arg++)
        {
	  PyObject *keyo=NULL;
          const char *key=sqlite3_bind_parameter_name(self->statement->vdbestatement, arg);

          if(!key)
            {
              PyErr_Format(ExcBindings, "Binding %d has no name, but you supplied a dict (which only has names).", arg-1);
              return -1;
            }

	  assert(*key==':' || *key=='$');
          key++; /* first char is a colon or dollar which we skip */

	  keyo=PyUnicode_DecodeUTF8(key, strlen(key), NULL);
	  if(!keyo) return -1;

	  obj=PyDict_GetItem(self->bindings, keyo);
	  Py_DECREF(keyo);

          if(!obj)
            /* this is where we could error on missing keys */
            continue;
          if(APSWCursor_dobinding(self,arg,obj)!=SQLITE_OK)
            {
              assert(PyErr_Occurred());
              return -1;
            }
        }

      return 0;
    }

  /* it must be a fast sequence */
  /* verify the number of args supplied */
  if (self->bindings)
    sz=PySequence_Fast_GET_SIZE(self->bindings);
  /* there is another statement after this one ... */
  if(self->statement->next && sz-self->bindingsoffset<nargs)
    {
      PyErr_Format(ExcBindings, "Incorrect number of bindings supplied.  The current statement uses %d and there are only %d left.  Current offset is %d",
                   nargs, (self->bindings)?sz:0, (int)(self->bindingsoffset));
      return -1;
    }
  /* no more statements */
  if(!self->statement->next && sz-self->bindingsoffset!=nargs)
    {
      PyErr_Format(ExcBindings, "Incorrect number of bindings supplied.  The current statement uses %d and there are %d supplied.  Current offset is %d",
                   nargs, (self->bindings)?sz:0, (int)(self->bindingsoffset));
      return -1;
    }
  
  res=SQLITE_OK;

  /* nb sqlite starts bind args at one not zero */
  for(arg=1;arg<=nargs;arg++)
    {
      obj=PySequence_Fast_GET_ITEM(self->bindings, arg-1+self->bindingsoffset);
      if(APSWCursor_dobinding(self, arg, obj))
        {
          assert(PyErr_Occurred());
          return -1;
        }
    }

  self->bindingsoffset+=nargs;
  assert(res==0);
  return 0;
}

static int
APSWCursor_doexectrace(APSWCursor *self, Py_ssize_t savedbindingsoffset)
{
  PyObject *retval=NULL;
  PyObject *sqlcmd=NULL;
  PyObject *bindings=NULL;
  int result;

  assert(self->exectrace);
  assert(self->statement);

  /* make a string of the command */
  sqlcmd=convertutf8buffersizetounicode(self->statement->utf8, self->statement->querylen);

  if(!sqlcmd) return -1;

  /* now deal with the bindings */
  if(self->bindings)
    {
      if(PyDict_Check(self->bindings))
        {
          bindings=self->bindings;
          Py_INCREF(self->bindings);
        }
      else
        {
          APSW_FAULT_INJECT(DoExecTraceBadSlice,
          bindings=PySequence_GetSlice(self->bindings, savedbindingsoffset, self->bindingsoffset),
          bindings=PyErr_NoMemory());

          if(!bindings)
            {
              Py_DECREF(sqlcmd);
              return -1;
            }
        }
    }
  else
    {
      bindings=Py_None;
      Py_INCREF(bindings);
    }

  retval=PyObject_CallFunction(self->exectrace, "OO", sqlcmd, bindings);
  Py_DECREF(sqlcmd);
  Py_DECREF(bindings);
  if(!retval) 
    {
      assert(PyErr_Occurred());
      return -1;
    }
  result=PyObject_IsTrue(retval);
  Py_DECREF(retval);
  assert (result==-1 || result==0 || result ==1);
  if(result==-1)
    {
      assert(PyErr_Occurred());
      return -1;
    }
  if(result)
    return 0;

  /* callback didn't want us to continue */
  PyErr_Format(ExcTraceAbort, "Aborted by false/null return value of exec tracer");
  return -1;
}

static PyObject*
APSWCursor_dorowtrace(APSWCursor *self, PyObject *retval)
{
  assert(self->rowtrace);

  retval=PyEval_CallObject(self->rowtrace, retval);
  if(!retval) 
    return NULL;
  
  return retval;
}

/* Returns a borrowed reference to self if all is ok, else NULL on error */
static PyObject *
APSWCursor_step(APSWCursor *self)
{
  int res;
  int savedbindingsoffset=0; /* initialised to stop stupid compiler from whining */

  for(;;)
    {
      assert(!PyErr_Occurred());
      APSW_BEGIN_ALLOW_THREADS
        res=(self->statement->vdbestatement)?(sqlite3_step(self->statement->vdbestatement)):(SQLITE_DONE);
      APSW_END_ALLOW_THREADS;

      switch(res&0xff)
        {
	case SQLITE_ROW:
          self->status=C_ROW;
          return (PyErr_Occurred())?(NULL):((PyObject*)self);

        case SQLITE_DONE:
	  if (PyErr_Occurred())
	    {
	      self->status=C_DONE;
	      return NULL;
	    }
          break;


	case SQLITE_SCHEMA:
	  /* We used to call statementcache_dup which did a reprepare.
             To avoid race conditions with the statement cache (we
             release the GIL around prepare now) we now just return
             the error.  See SQLite ticket 2158.

             ::TODO:: with the new fangled statementcache it is safe
             to do a reprepare
	   */

        default: /* sqlite3_prepare_v2 introduced in 3.3.9 means the
		    error code is returned from step as well as
		    finalize/reset */
          /* FALLTHRU */
        case SQLITE_ERROR:  /* SQLITE_BUSY is handled here as well */
          /* there was an error - we need to get actual error code from sqlite3_finalize */
          self->status=C_DONE;
          if(PyErr_Occurred())
            /* we don't care about further errors from the sql */
            resetcursor(self, 1);
          else
            {
              res=resetcursor(self, 0);  /* this will get the error code for us */
              assert(res!=SQLITE_OK);
            }
          return NULL;

          
        }
      assert(res==SQLITE_DONE);

      /* done with that statement, are there any more? */
      self->status=C_DONE;
      if(!self->statement->next)
        {
          PyObject *next;

          /* in executemany mode ?*/
          if(!self->emiter)
            {
              /* no more so we finalize */
              if(resetcursor(self, 0)!=SQLITE_OK)
                {
                  assert(PyErr_Occurred());
                  return NULL; /* exception */
                }
              return (PyObject*)self;
            }

          /* we are in executemany mode */
          next=PyIter_Next(self->emiter);
          if(PyErr_Occurred())
            return NULL;
          
          if(!next)
            {
              /* clear out statement if no more*/
              if(resetcursor(self, 0)!=SQLITE_OK)
                {
                  assert(PyErr_Occurred());
                  return NULL;
                }

            return (PyObject*)self;
            }

          /* we need to clear just completed and restart original executemany statement */
          statementcache_finalize(self->connection->stmtcache, self->statement);
          self->statement=NULL;
          /* don't need bindings from last round if emiter.next() */
          Py_XDECREF(self->bindings);
          self->bindings=0;
          self->bindingsoffset=0;
          /* verify type of next before putting in bindings */
          if(PyDict_Check(next))
            self->bindings=next;
          else
            {
              self->bindings=PySequence_Fast(next, "You must supply a dict or a sequence");
              /* we no longer need next irrespective of what happens in line above */
              Py_DECREF(next);
              if(!self->bindings)
                return NULL;
            }
          assert(self->bindings);
        }

      /* finalise and go again */
      self->inuse=1;
      if(!self->statement)
        {
          /* we are going again in executemany mode */
          assert(self->emiter);
          self->statement=statementcache_prepare(self->connection->stmtcache, self->emoriginalquery);
          res=(self->statement)?SQLITE_OK:SQLITE_ERROR;
        }
      else
        {
          /* next sql statement */
          res=statementcache_next(self->connection->stmtcache, &self->statement);
          SET_EXC(res, self->connection->db);
        }
      self->inuse=0;

      if (res!=SQLITE_OK)
        {
          assert((res&0xff)!=SQLITE_BUSY); /* finalize shouldn't be returning busy, only step */
          assert(!self->statement);
          return NULL;
        }

      assert(self->statement);
      if(self->exectrace)
        {
          savedbindingsoffset=self->bindingsoffset;
        }
      assert(!PyErr_Occurred());

      if(APSWCursor_dobindings(self))
        {
          assert(PyErr_Occurred());
          return NULL;
        }

      if(self->exectrace)
        {
          if(APSWCursor_doexectrace(self, savedbindingsoffset))
            {
              assert(self->status==C_DONE);
              assert(PyErr_Occurred());
              return NULL;
            }
        }
      assert(self->status==C_DONE);
      self->status=C_BEGIN;
    }

  /* you can't actually get here */
  assert(0);
  return NULL;
}

static PyObject *
APSWCursor_execute(APSWCursor *self, PyObject *args)
{
  int res;
  int savedbindingsoffset;
  PyObject *retval=NULL;
  PyObject *query;

  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  res=resetcursor(self, /* force= */ 0);
  if(res!=SQLITE_OK)
    {
      assert(PyErr_Occurred());
      return NULL;
    }
  
  assert(!self->bindings);
  assert(PyTuple_Check(args));

  if(PyTuple_GET_SIZE(args)<1 || PyTuple_GET_SIZE(args)>2)
    {
      PyErr_Format(PyExc_TypeError, "Incorrect number of arguments.  execute(statements [,bindings])");
      return NULL;
    }

  query=PyTuple_GET_ITEM(args, 0);
  if (PyTuple_GET_SIZE(args)==2)
    self->bindings=PyTuple_GET_ITEM(args, 1);

  if(self->bindings)
    {
      if(PyDict_Check(self->bindings))
        Py_INCREF(self->bindings);
      else
        {
          self->bindings=PySequence_Fast(self->bindings, "You must supply a dict or a sequence");
          if(!self->bindings)
            return NULL;
        }
    }

  assert(!self->statement);
  assert(!PyErr_Occurred());
  self->inuse=1;
  self->statement=statementcache_prepare(self->connection->stmtcache, query);
  self->inuse=0;
  if (!self->statement)
    {
      AddTraceBackHere(__FILE__, __LINE__, "APSWCursor_execute.sqlite3_prepare_v2", "{s: O, s: O}", 
		       "Connection", self->connection, 
		       "statement", query);
      return NULL;
    }
  assert(!PyErr_Occurred());

  self->bindingsoffset=0;
  if(self->exectrace)
    savedbindingsoffset=0;

  if(APSWCursor_dobindings(self))
    {
      assert(PyErr_Occurred());
      return NULL;
    }

  if(self->exectrace)
    {
      if(APSWCursor_doexectrace(self, savedbindingsoffset))
        {
          assert(PyErr_Occurred());
          return NULL;  
        }
    }

  self->status=C_BEGIN;

  retval=APSWCursor_step(self);
  if (!retval) 
    {
      assert(PyErr_Occurred());
      return NULL;
    }
  Py_INCREF(retval);
  return retval;
}

static PyObject *
APSWCursor_executemany(APSWCursor *self, PyObject *args)
{
  int res;
  PyObject *retval=NULL;
  PyObject *theiterable=NULL;
  PyObject *next=NULL;
  PyObject *query=NULL;
  int savedbindingsoffset;

  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  res=resetcursor(self, /* force= */ 0);
  if(res!=SQLITE_OK)
    {
      assert(PyErr_Occurred());
      return NULL;
    }
  
  assert(!self->bindings);
  assert(!self->emiter);
  assert(!self->emoriginalquery);
  assert(self->status=C_DONE);

  if(!PyArg_ParseTuple(args, "OO:executemany(statements, sequenceofbindings)", &query, &theiterable))
    return NULL;

  self->emiter=PyObject_GetIter(theiterable);
  if (!self->emiter)
    {
      PyErr_Format(PyExc_TypeError, "2nd parameter must be iterable");
      return NULL;
    }

  next=PyIter_Next(self->emiter);
  if(!next && PyErr_Occurred())
    return NULL;
  if(!next)
    {
      /* empty list */
      Py_INCREF(self);
      return (PyObject*)self;
    }

  if(PyDict_Check(next))
    self->bindings=next;
  else
    {
      self->bindings=PySequence_Fast(next, "You must supply a dict or a sequence");
      Py_DECREF(next); /* _Fast makes new reference */
      if(!self->bindings)
          return NULL;
    }

  assert(!self->statement);
  assert(!PyErr_Occurred());
  assert(!self->statement);
  self->inuse=1;
  self->statement=statementcache_prepare(self->connection->stmtcache, query);
  self->inuse=0;
  if (!self->statement)
    {
      AddTraceBackHere(__FILE__, __LINE__, "APSWCursor_executemany.sqlite3_prepare_v2", "{s: O, s: O}", 
		       "Connection", self->connection, 
		       "statement", query);
      return NULL;
    }
  assert(!PyErr_Occurred());

  self->emoriginalquery=self->statement->utf8;
  Py_INCREF(self->emoriginalquery);

  self->bindingsoffset=0;
  if(self->exectrace)
    savedbindingsoffset=0;

  if(APSWCursor_dobindings(self))
    {
      assert(PyErr_Occurred());
      return NULL;
    }

  if(self->exectrace)
    {
      if(APSWCursor_doexectrace(self, savedbindingsoffset))
        {
          assert(PyErr_Occurred());
          return NULL;  
        }
    }

  self->status=C_BEGIN;

  retval=APSWCursor_step(self);
  if (!retval) 
    {
      assert(PyErr_Occurred());
      return NULL;
    }
  Py_INCREF(retval);
  return retval;
}

static PyObject *
APSWCursor_close(APSWCursor *self, PyObject *args)
{
  int res;
  int force=0;

  CHECK_USE(NULL);
  if (!self->connection->db) /* if connection is closed, then we must also be closed */
    Py_RETURN_NONE;

  if(!PyArg_ParseTuple(args, "|i:close(force=False)", &force))
    return NULL;

  res=resetcursor(self, force);
  if(res!=SQLITE_OK)
    {
      assert(PyErr_Occurred());
      return NULL;
    }
  Py_RETURN_NONE;
}

static PyObject *
APSWCursor_next(APSWCursor *self)
{
  PyObject *retval;
  PyObject *item;
  int numcols=-1;
  int i;

  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

 again:
  if(self->status==C_BEGIN)
    if(!APSWCursor_step(self))
      {
        assert(PyErr_Occurred());
        return NULL;
      }
  if(self->status==C_DONE)
    return NULL;

  assert(self->status==C_ROW);

  self->status=C_BEGIN;
  
  /* return the row of data */
  numcols=sqlite3_data_count(self->statement->vdbestatement);
  retval=PyTuple_New(numcols);
  if(!retval) goto error;

  for(i=0;i<numcols;i++)
    {
      item=convert_column_to_pyobject(self->statement->vdbestatement, i);
      if(!item) goto error;
      PyTuple_SET_ITEM(retval, i, item);
    }
  if(self->rowtrace)
    {
      PyObject *r2=APSWCursor_dorowtrace(self, retval);
      Py_DECREF(retval);
      if(!r2) 
	return NULL;
      if (r2==Py_None)
        {
          Py_DECREF(r2);
          goto again;
        }
      return r2;
    }
  return retval;
 error:
  Py_XDECREF(retval);
  return NULL;
}

static PyObject *
APSWCursor_iter(APSWCursor *self)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  Py_INCREF(self);
  return (PyObject*)self;
}

static PyObject *
APSWCursor_setexectrace(APSWCursor *self, PyObject *func)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  if(func!=Py_None && !PyCallable_Check(func))
    {
      PyErr_SetString(PyExc_TypeError, "parameter must be callable");
      return NULL;
    }

  if(func!=Py_None)
    Py_INCREF(func);

  Py_XDECREF(self->exectrace);
  self->exectrace=(func!=Py_None)?func:NULL;

  Py_RETURN_NONE;
}

static PyObject *
APSWCursor_setrowtrace(APSWCursor *self, PyObject *func)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  if(func!=Py_None && !PyCallable_Check(func))
    {
      PyErr_SetString(PyExc_TypeError, "parameter must be callable");
      return NULL;
    }

  if(func!=Py_None)
    Py_INCREF(func);

  Py_XDECREF(self->rowtrace);
  self->rowtrace=(func!=Py_None)?func:NULL;

  Py_RETURN_NONE;
}

static PyObject *
APSWCursor_getexectrace(APSWCursor *self)
{
  PyObject *ret;

  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  ret=(self->exectrace)?(self->exectrace):Py_None;
  Py_INCREF(ret);
  return ret;
}

static PyObject *
APSWCursor_getrowtrace(APSWCursor *self)
{
  PyObject *ret;
  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);
  ret =(self->rowtrace)?(self->rowtrace):Py_None;
  Py_INCREF(ret);
  return ret;
}

static PyObject *
APSWCursor_getconnection(APSWCursor *self)
{
  CHECK_USE(NULL);
  CHECK_CLOSED(self->connection, NULL);

  Py_INCREF(self->connection);
  return (PyObject*)self->connection;
}

static PyMethodDef APSWCursor_methods[] = {
  {"execute", (PyCFunction)APSWCursor_execute, METH_VARARGS,
   "Executes one or more statements" },
  {"executemany", (PyCFunction)APSWCursor_executemany, METH_VARARGS,
   "Repeatedly executes statements on sequence" },
  {"setexectrace", (PyCFunction)APSWCursor_setexectrace, METH_O,
   "Installs a function called for every statement executed"},
  {"setrowtrace", (PyCFunction)APSWCursor_setrowtrace, METH_O,
   "Installs a function called for every row returned"},
  {"getexectrace", (PyCFunction)APSWCursor_getexectrace, METH_NOARGS,
   "Returns the current exec tracer function"},
  {"getrowtrace", (PyCFunction)APSWCursor_getrowtrace, METH_NOARGS,
   "Returns the current row tracer function"},
  {"getrowtrace", (PyCFunction)APSWCursor_getrowtrace, METH_NOARGS,
   "Returns the current row tracer function"},
  {"getconnection", (PyCFunction)APSWCursor_getconnection, METH_NOARGS,
   "Returns the connection object for this cursor"},
  {"getdescription", (PyCFunction)APSWCursor_getdescription, METH_NOARGS,
   "Returns the description for the current row"},
  {"close", (PyCFunction)APSWCursor_close, METH_VARARGS,
   "Closes the cursor" },
  {0, 0, 0, 0}  /* Sentinel */
};


static PyTypeObject APSWCursorType = {
#if PY_MAJOR_VERSION < 3
    PyObject_HEAD_INIT(NULL)
    0,                         /*ob_size*/
#else
    PyVarObject_HEAD_INIT(NULL,0)
#endif
    "apsw.Cursor",             /*tp_name*/
    sizeof(APSWCursor),            /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)APSWCursor_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_VERSION_TAG
#if PY_MAJOR_VERSION < 3
 | Py_TPFLAGS_HAVE_ITER
#endif
 , /*tp_flags*/
    "Cursor object",           /* tp_doc */
    0,		               /* tp_traverse */
    0,		               /* tp_clear */
    0,		               /* tp_richcompare */
    0,		               /* tp_weaklistoffset */
    (getiterfunc)APSWCursor_iter,  /* tp_iter */
    (iternextfunc)APSWCursor_next, /* tp_iternext */
    APSWCursor_methods,            /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    0,                         /* tp_new */
    0,                         /* tp_free */
    0,                         /* tp_is_gc */
    0,                         /* tp_bases */
    0,                         /* tp_mro */
    0,                         /* tp_cache */
    0,                         /* tp_subclasses */
    0,                         /* tp_weaklist */
    0,                         /* tp_del */
#if PY_VERSION_HEX>=0x02060000
    0                          /* tp_version_tag */
#endif
};

#include "vfs.c"


/* MODULE METHODS */
static PyObject *
getsqliteversion(void)
{
  return MAKESTR(sqlite3_libversion());
}

static PyObject *
getapswversion(void)
{
  return MAKESTR(APSW_VERSION);
}

static PyObject *
enablesharedcache(APSW_ARGUNUSED PyObject *self, PyObject *args)
{
  int setting,res;
  if(!PyArg_ParseTuple(args, "i:enablesharedcache(boolean)", &setting))
    return NULL;

  APSW_FAULT_INJECT(EnableSharedCacheFail,res=sqlite3_enable_shared_cache(setting),res=SQLITE_NOMEM);
  SET_EXC(res, NULL);

  if(res!=SQLITE_OK)
    return NULL;

  Py_RETURN_NONE;
}

static PyObject *
initialize(void)
{
  int res;

  res=sqlite3_initialize();
  APSW_FAULT_INJECT(InitializeFail, ,res=SQLITE_NOMEM);
  SET_EXC(res, NULL);

  if(res!=SQLITE_OK)
    return NULL;

  Py_RETURN_NONE;
}

static PyObject *
sqliteshutdown(void)
{
  int res;
  
  APSW_FAULT_INJECT(ShutdownFail, res=sqlite3_shutdown(), res=SQLITE_NOMEM);
  SET_EXC(res, NULL);

  if(res!=SQLITE_OK)
    return NULL;

  Py_RETURN_NONE;
}

#ifdef EXPERIMENTAL
static PyObject *
config(APSW_ARGUNUSED PyObject *self, PyObject *args)
{
  int res, optdup;
  long opt;

  if(PyTuple_GET_SIZE(args)<1 || !PyIntLong_Check(PyTuple_GET_ITEM(args, 0)))
    {
      PyErr_Format(PyExc_TypeError, "There should be at least one argument with the first being a number");
      return NULL;
    }
  opt=PyIntLong_AsLong(PyTuple_GET_ITEM(args,0));
  if(PyErr_Occurred())
    return NULL;

  switch(opt)
    {
    case SQLITE_CONFIG_SINGLETHREAD:
    case SQLITE_CONFIG_MULTITHREAD:
    case SQLITE_CONFIG_SERIALIZED:
      if(!PyArg_ParseTuple(args, "i", &optdup))
        return NULL;
      assert(opt==optdup);
      res=sqlite3_config( (int)opt );
      break;
      
    case SQLITE_CONFIG_MEMSTATUS:
      {
        int boolval;
        if(!PyArg_ParseTuple(args, "ii", &optdup, &boolval))
          return NULL;
        assert(opt==optdup);
        res=sqlite3_config( (int)opt, boolval);
        break;
      }
      
    default:
      PyErr_Format(PyExc_TypeError, "Unknown config type %d", (int)opt);
      return NULL;
    }

  SET_EXC(res, NULL);

  if(res!=SQLITE_OK)
    return NULL;

  Py_RETURN_NONE;
}
#endif /* EXPERIMENTAL */

static PyObject*
memoryused(void)
{
  return PyLong_FromLongLong(sqlite3_memory_used());
}

static PyObject*
memoryhighwater(APSW_ARGUNUSED PyObject *self, PyObject *args)
{
  int reset=0;

  if(!PyArg_ParseTuple(args, "|i:memoryhighwater(reset=False)", &reset))
    return NULL;

  return PyLong_FromLongLong(sqlite3_memory_highwater(reset));
}

static PyObject*
softheaplimit(APSW_ARGUNUSED PyObject *self, PyObject *args)
{
  int limit;

  if(!PyArg_ParseTuple(args, "i", &limit))
    return NULL;

  sqlite3_soft_heap_limit(limit);

  Py_RETURN_NONE;
}

static PyObject*
randomness(APSW_ARGUNUSED PyObject *self, PyObject *args)
{
  int amount;
  PyObject *bytes;

  if(!PyArg_ParseTuple(args, "i", &amount))
    return NULL;
  if(amount<0)
    {
      PyErr_Format(PyExc_ValueError, "Can't have negative number of bytes");
      return NULL;
    }
  bytes=PyBytes_FromStringAndSize(NULL, amount);
  if(!bytes) return bytes;
  sqlite3_randomness(amount, PyBytes_AS_STRING(bytes));
  return bytes;
}

static PyObject*
releasememory(APSW_ARGUNUSED PyObject *self, PyObject *args)
{
  int amount;

  if(!PyArg_ParseTuple(args, "i", &amount))
    return NULL;

  return PyInt_FromLong(sqlite3_release_memory(amount));
}

static PyObject *
status(APSW_ARGUNUSED PyObject *self, PyObject *args)
{
  int res, op, current=0, highwater=0, reset=0;

  if(!PyArg_ParseTuple(args, "i|i:status(op, reset=False)", &op, &reset))
    return NULL;

  res=sqlite3_status(op, &current, &highwater, reset);
  SET_EXC(res, NULL);

  if(res!=SQLITE_OK)
    return NULL;

  return Py_BuildValue("(ii)", current, highwater);
}

static PyObject *
vfsnames(APSW_ARGUNUSED PyObject *self)
{
  PyObject *result=NULL, *str=NULL;
  sqlite3_vfs *vfs=sqlite3_vfs_find(0);

  result=PyList_New(0);
  if(!result) goto error;

  while(vfs)
    {
      APSW_FAULT_INJECT(vfsnamesfails, 
                        str=convertutf8string(vfs->zName),
                        str=PyErr_NoMemory());
      if(!str) goto error;
      if(PyList_Append(result, str)) goto error;
      Py_DECREF(str);
      vfs=vfs->pNext;
    }
  return result;

 error:
  Py_XDECREF(str);
  Py_XDECREF(result);
  return NULL;
}

static PyObject *
getapswexceptionfor(APSW_ARGUNUSED PyObject *self, PyObject *pycode)
{
  int code, i;
  PyObject *result=NULL;

  if(!PyIntLong_Check(pycode))
    {
      PyErr_Format(PyExc_TypeError, "Argument should be an integer");
      return NULL;
    }
  code=PyIntLong_AsLong(pycode);
  if(PyErr_Occurred()) return NULL;

  for(i=0;exc_descriptors[i].name;i++)
    if (exc_descriptors[i].code==(code&0xff))
      {
        result=PyObject_CallObject(exc_descriptors[i].cls, NULL);
        if(!result) return result;
        break;
      }
  if(!result)
    {
      PyErr_Format(PyExc_ValueError, "%d is not a known error code", code);
      return result;
    }

  PyObject_SetAttrString(result, "extendedresult", PyInt_FromLong(code));
  PyObject_SetAttrString(result, "result", PyInt_FromLong(code&0xff));
  return result;
}

#if defined(APSW_TESTFIXTURES) && defined(APSW_USE_SQLITE_AMALGAMATION)
/* a routine to reset the random number generator so that we can test xRandomness */
static PyObject *
apsw_test_reset_rng(APSW_ARGUNUSED PyObject *self)
{
  /* See sqlite3PrngResetState in sqlite's random.c which is above us if using the amalgamation */
  GLOBAL(struct sqlite3PrngType, sqlite3Prng).isInit = 0;

  Py_RETURN_NONE;
}
#endif

#ifdef APSW_TESTFIXTURES
/* xGetLastError isn't actually called anywhere by SQLite so add a
   manual way of doing so
   http://www.sqlite.org/cvstrac/tktview?tn=3337 */

static PyObject *
apsw_call_xGetLastError(APSW_ARGUNUSED PyObject *self, PyObject *args)
{
  char *vfsname;
  int bufsize;
  PyObject *resultbuffer=NULL;
  sqlite3_vfs *vfs;
  int res=-1;

  if(!PyArg_ParseTuple(args, "esi", STRENCODING, &vfsname, &bufsize))
    return NULL;

  vfs=sqlite3_vfs_find(vfsname);
  if(!vfs) goto finally;

  resultbuffer=PyBytes_FromStringAndSize(NULL, bufsize);
  if(!resultbuffer) goto finally;

  memset(PyBytes_AS_STRING(resultbuffer), 0, PyBytes_GET_SIZE(resultbuffer));

  res=vfs->xGetLastError(vfs, bufsize, PyBytes_AS_STRING(resultbuffer));

 finally:
  if(vfsname)
    PyMem_Free(vfsname);

  return resultbuffer?Py_BuildValue("Ni", resultbuffer, res):NULL;
}

static PyObject *
apsw_fini(APSW_ARGUNUSED PyObject *self)
{
  APSWBuffer_fini();

  Py_RETURN_NONE;
}
#endif


static PyMethodDef module_methods[] = {
  {"sqlitelibversion", (PyCFunction)getsqliteversion, METH_NOARGS,
   "Return the version of the SQLite library"},
  {"apswversion", (PyCFunction)getapswversion, METH_NOARGS,
   "Return the version of the APSW wrapper"},
  {"vfsnames", (PyCFunction)vfsnames, METH_NOARGS,
   "Returns list of vfs names"},
  {"enablesharedcache", (PyCFunction)enablesharedcache, METH_VARARGS,
   "Sets shared cache semantics for this thread"},
  {"initialize", (PyCFunction)initialize, METH_NOARGS,
   "Initialize SQLite library"},
  {"shutdown", (PyCFunction)sqliteshutdown, METH_NOARGS,
   "Shutdown SQLite library"},
#ifdef EXPERIMENTAL
  {"config", (PyCFunction)config, METH_VARARGS,
   "Calls sqlite3_config"},
#endif
  {"memoryused", (PyCFunction)memoryused, METH_NOARGS,
   "Current SQLite memory in use"},
  {"memoryhighwater", (PyCFunction)memoryhighwater, METH_VARARGS,
   "Most amount of memory used"},
  {"status", (PyCFunction)status, METH_VARARGS,
   "Gets various SQLite counters"},
  {"softheaplimit", (PyCFunction)softheaplimit, METH_VARARGS,
   "Sets soft limit on SQLite memory usage"},
  {"releasememory", (PyCFunction)releasememory, METH_VARARGS,
   "Attempts to free specified amount of memory"},
  {"randomness", (PyCFunction)randomness, METH_VARARGS,
   "Obtains random bytes"},
  {"exceptionfor", (PyCFunction)getapswexceptionfor, METH_O,
   "Returns exception instance corresponding to supplied sqlite error code"},
#if defined(APSW_TESTFIXTURES) && defined(APSW_USE_SQLITE_AMALGAMATION)
  {"test_reset_rng", (PyCFunction)apsw_test_reset_rng, METH_NOARGS,
   "Resets random number generator so we can test vfs xRandomness"},
#endif
#ifdef APSW_TESTFIXTURES
  {"test_call_xGetLastError", (PyCFunction)apsw_call_xGetLastError, METH_VARARGS,
   "Calls xGetLastError routine"},
  {"_fini", (PyCFunction)apsw_fini, METH_NOARGS,
   "Frees all caches and recycle lists"},
#endif
  {0, 0, 0, 0}  /* Sentinel */
};



#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef apswmoduledef={
  PyModuleDef_HEAD_INIT,
  "apsw", 
  NULL,
  -1,
  module_methods,
  0,
  0,
  0,
  0,
};
#endif


PyMODINIT_FUNC
#if PY_MAJOR_VERSION < 3
initapsw(void) 
#else
PyInit_apsw(void)
#endif
{
    PyObject *m=NULL;
    PyObject *thedict=NULL;
    const char *mapping_name=NULL;
    PyObject *hooks;
    unsigned int i;

    assert(sizeof(int)==4);             /* we expect 32 bit ints */
    assert(sizeof(long long)==8);             /* we expect 64 bit long long */

    /* Check SQLite was compiled with thread safety */
    if(!sqlite3_threadsafe())
      {
        PyErr_Format(PyExc_EnvironmentError, "SQLite was compiled without thread safety and cannot be used.");
        goto fail;
      }

    if (PyType_Ready(&ConnectionType) < 0
        || PyType_Ready(&APSWCursorType) < 0
        || PyType_Ready(&ZeroBlobBindType) <0
        || PyType_Ready(&APSWBlobType) <0
        || PyType_Ready(&APSWVFSType) <0
        || PyType_Ready(&APSWVFSFileType) <0
        || PyType_Ready(&APSWStatementType) <0
        || PyType_Ready(&APSWBufferType) <0
        )
      goto fail;

    /* ensure threads are available */
    PyEval_InitThreads();

#if PY_MAJOR_VERSION < 3
    m = apswmodule = Py_InitModule3("apsw", module_methods,
                       "Another Python SQLite Wrapper.");
#else
    m = apswmodule = PyModule_Create(&apswmoduledef);
#endif

    if (m == NULL)  goto fail;

    if(init_exceptions(m)) goto fail;

    Py_INCREF(&ConnectionType);
    PyModule_AddObject(m, "Connection", (PyObject *)&ConnectionType);
    
    /* we don't add cursor to the module since users shouldn't be able to instantiate them directly */
    
    Py_INCREF(&ZeroBlobBindType);
    PyModule_AddObject(m, "zeroblob", (PyObject *)&ZeroBlobBindType);

    Py_INCREF(&APSWVFSType);
    PyModule_AddObject(m, "VFS", (PyObject*)&APSWVFSType);
    Py_INCREF(&APSWVFSFileType);
    PyModule_AddObject(m, "VFSFile", (PyObject*)&APSWVFSFileType);
    
    hooks=PyList_New(0);
    if(!hooks) goto fail;
    PyModule_AddObject(m, "connection_hooks", hooks);

    /* Version number */
    PyModule_AddIntConstant(m, "SQLITE_VERSION_NUMBER", SQLITE_VERSION_NUMBER);
    

    /* add in some constants and also put them in a corresponding mapping dictionary */

    /* sentinel should be a number that doesn't exist */
#define SENTINEL -786343
#define DICT(n) {n, SENTINEL}
#define END {NULL, 0}
#define ADDINT(n) {#n, n}

    struct { const char *name; int value; } integers[]={
      DICT("mapping_authorizer_return"),
      ADDINT(SQLITE_DENY),
      ADDINT(SQLITE_IGNORE),
      ADDINT(SQLITE_OK),
      END,
      
      DICT("mapping_authorizer_function"),
      ADDINT(SQLITE_CREATE_INDEX),
      ADDINT(SQLITE_CREATE_TABLE),
      ADDINT(SQLITE_CREATE_TEMP_INDEX),
      ADDINT(SQLITE_CREATE_TEMP_TABLE),
      ADDINT(SQLITE_CREATE_TEMP_TRIGGER),
      ADDINT(SQLITE_CREATE_TEMP_VIEW),
      ADDINT(SQLITE_CREATE_TRIGGER),
      ADDINT(SQLITE_CREATE_VIEW),
      ADDINT(SQLITE_DELETE),
      ADDINT(SQLITE_DROP_INDEX),
      ADDINT(SQLITE_DROP_TABLE),
      ADDINT(SQLITE_DROP_TEMP_INDEX),
      ADDINT(SQLITE_DROP_TEMP_TABLE),
      ADDINT(SQLITE_DROP_TEMP_TRIGGER),
      ADDINT(SQLITE_DROP_TEMP_VIEW),
      ADDINT(SQLITE_DROP_TRIGGER),
      ADDINT(SQLITE_DROP_VIEW),
      ADDINT(SQLITE_INSERT),
      ADDINT(SQLITE_PRAGMA),
      ADDINT(SQLITE_READ),
      ADDINT(SQLITE_SELECT),
      ADDINT(SQLITE_TRANSACTION),
      ADDINT(SQLITE_UPDATE),
      ADDINT(SQLITE_ATTACH),
      ADDINT(SQLITE_DETACH),
      ADDINT(SQLITE_ALTER_TABLE),
      ADDINT(SQLITE_REINDEX),
      ADDINT(SQLITE_COPY),
      ADDINT(SQLITE_ANALYZE),
      ADDINT(SQLITE_CREATE_VTABLE),
      ADDINT(SQLITE_DROP_VTABLE),
      ADDINT(SQLITE_FUNCTION),
      END,

      /* vtable best index constraints */
#if defined(SQLITE_INDEX_CONSTRAINT_EQ) && defined(SQLITE_INDEX_CONSTRAINT_MATCH)
      DICT("mapping_bestindex_constraints"),
      ADDINT(SQLITE_INDEX_CONSTRAINT_EQ),
      ADDINT(SQLITE_INDEX_CONSTRAINT_GT),
      ADDINT(SQLITE_INDEX_CONSTRAINT_LE),
      ADDINT(SQLITE_INDEX_CONSTRAINT_LT),
      ADDINT(SQLITE_INDEX_CONSTRAINT_GE),
      ADDINT(SQLITE_INDEX_CONSTRAINT_MATCH),
      END,
#endif /* constraints */

    /* extendended result codes */
      DICT("mapping_extended_result_codes"),
      ADDINT(SQLITE_IOERR_READ),
      ADDINT(SQLITE_IOERR_SHORT_READ),
      ADDINT(SQLITE_IOERR_WRITE),
      ADDINT(SQLITE_IOERR_FSYNC),
      ADDINT(SQLITE_IOERR_DIR_FSYNC),
      ADDINT(SQLITE_IOERR_TRUNCATE),
      ADDINT(SQLITE_IOERR_FSTAT),
      ADDINT(SQLITE_IOERR_UNLOCK),
      ADDINT(SQLITE_IOERR_RDLOCK),
      ADDINT(SQLITE_IOERR_DELETE),
      ADDINT(SQLITE_IOERR_BLOCKED),
      ADDINT(SQLITE_IOERR_NOMEM),
      ADDINT(SQLITE_IOERR_ACCESS),
      ADDINT(SQLITE_IOERR_CHECKRESERVEDLOCK),
      ADDINT(SQLITE_IOERR_LOCK),
      END,

    /* error codes */
      DICT("mapping_result_codes"),
      ADDINT(SQLITE_OK),
      ADDINT(SQLITE_ERROR),
      ADDINT(SQLITE_INTERNAL),
      ADDINT(SQLITE_PERM),
      ADDINT(SQLITE_ABORT),
      ADDINT(SQLITE_BUSY),
      ADDINT(SQLITE_LOCKED),
      ADDINT(SQLITE_NOMEM),
      ADDINT(SQLITE_READONLY),
      ADDINT(SQLITE_INTERRUPT),
      ADDINT(SQLITE_IOERR),
      ADDINT(SQLITE_CORRUPT),
      ADDINT(SQLITE_FULL),
      ADDINT(SQLITE_CANTOPEN),
      ADDINT(SQLITE_PROTOCOL),
      ADDINT(SQLITE_EMPTY),
      ADDINT(SQLITE_SCHEMA),
      ADDINT(SQLITE_CONSTRAINT),
      ADDINT(SQLITE_MISMATCH),
      ADDINT(SQLITE_MISUSE),
      ADDINT(SQLITE_NOLFS),
      ADDINT(SQLITE_AUTH),
      ADDINT(SQLITE_FORMAT),
      ADDINT(SQLITE_RANGE),
      ADDINT(SQLITE_NOTADB),
      END,

      /* open flags */
      DICT("mapping_open_flags"),
      ADDINT(SQLITE_OPEN_READONLY),
      ADDINT(SQLITE_OPEN_READWRITE),
      ADDINT(SQLITE_OPEN_CREATE),
      ADDINT(SQLITE_OPEN_DELETEONCLOSE),
      ADDINT(SQLITE_OPEN_EXCLUSIVE),
      ADDINT(SQLITE_OPEN_MAIN_DB),
      ADDINT(SQLITE_OPEN_TEMP_DB),
      ADDINT(SQLITE_OPEN_TRANSIENT_DB),
      ADDINT(SQLITE_OPEN_MAIN_JOURNAL),
      ADDINT(SQLITE_OPEN_TEMP_JOURNAL),
      ADDINT(SQLITE_OPEN_SUBJOURNAL),
      ADDINT(SQLITE_OPEN_MASTER_JOURNAL),
      ADDINT(SQLITE_OPEN_NOMUTEX),
      ADDINT(SQLITE_OPEN_FULLMUTEX),
      END,

      /* limits */
      DICT("mapping_limits"),
      ADDINT(SQLITE_LIMIT_LENGTH),
      ADDINT(SQLITE_LIMIT_SQL_LENGTH),
      ADDINT(SQLITE_LIMIT_COLUMN),
      ADDINT(SQLITE_LIMIT_EXPR_DEPTH),
      ADDINT(SQLITE_LIMIT_COMPOUND_SELECT),
      ADDINT(SQLITE_LIMIT_VDBE_OP),
      ADDINT(SQLITE_LIMIT_FUNCTION_ARG),
      ADDINT(SQLITE_LIMIT_ATTACHED),
      ADDINT(SQLITE_LIMIT_LIKE_PATTERN_LENGTH),
      ADDINT(SQLITE_LIMIT_VARIABLE_NUMBER),
      /* We don't include the MAX limits - see http://code.google.com/p/apsw/issues/detail?id=17 */
      END,

      DICT("mapping_config"),
      ADDINT(SQLITE_CONFIG_SINGLETHREAD),
      ADDINT(SQLITE_CONFIG_MULTITHREAD),
      ADDINT(SQLITE_CONFIG_SERIALIZED),
      ADDINT(SQLITE_CONFIG_MALLOC),
      ADDINT(SQLITE_CONFIG_GETMALLOC),
      ADDINT(SQLITE_CONFIG_SCRATCH),
      ADDINT(SQLITE_CONFIG_PAGECACHE),
      ADDINT(SQLITE_CONFIG_HEAP),
      ADDINT(SQLITE_CONFIG_MEMSTATUS),
      ADDINT(SQLITE_CONFIG_MUTEX),
      ADDINT(SQLITE_CONFIG_GETMUTEX),
      ADDINT(SQLITE_CONFIG_CHUNKALLOC),
      ADDINT(SQLITE_CONFIG_LOOKASIDE),
      END,

      DICT("mapping_db_config"),
      ADDINT(SQLITE_DBCONFIG_LOOKASIDE),
      END,

      DICT("mapping_status"),
      ADDINT(SQLITE_STATUS_MEMORY_USED),
      ADDINT(SQLITE_STATUS_PAGECACHE_USED),
      ADDINT(SQLITE_STATUS_PAGECACHE_OVERFLOW),
      ADDINT(SQLITE_STATUS_SCRATCH_USED),
      ADDINT(SQLITE_STATUS_SCRATCH_OVERFLOW),
      ADDINT(SQLITE_STATUS_MALLOC_SIZE),
      ADDINT(SQLITE_STATUS_PARSER_STACK),
      ADDINT(SQLITE_STATUS_PAGECACHE_SIZE),
      ADDINT(SQLITE_STATUS_SCRATCH_SIZE),
      END,

      DICT("mapping_db_status"),
      ADDINT(SQLITE_DBSTATUS_LOOKASIDE_USED),
      END,

      DICT("mapping_locking_level"),
      ADDINT(SQLITE_LOCK_NONE),
      ADDINT(SQLITE_LOCK_SHARED),
      ADDINT(SQLITE_LOCK_RESERVED),
      ADDINT(SQLITE_LOCK_PENDING),
      ADDINT(SQLITE_LOCK_EXCLUSIVE),
      END,

      DICT("mapping_access"),
      ADDINT(SQLITE_ACCESS_EXISTS),
      ADDINT(SQLITE_ACCESS_READWRITE),
      ADDINT(SQLITE_ACCESS_READ),
      END,

      DICT("mapping_device_characteristics"),
      ADDINT(SQLITE_IOCAP_ATOMIC),
      ADDINT(SQLITE_IOCAP_ATOMIC512),
      ADDINT(SQLITE_IOCAP_ATOMIC1K),
      ADDINT(SQLITE_IOCAP_ATOMIC2K),
      ADDINT(SQLITE_IOCAP_ATOMIC4K),
      ADDINT(SQLITE_IOCAP_ATOMIC8K),
      ADDINT(SQLITE_IOCAP_ATOMIC16K),
      ADDINT(SQLITE_IOCAP_ATOMIC32K),
      ADDINT(SQLITE_IOCAP_ATOMIC64K),
      ADDINT(SQLITE_IOCAP_SAFE_APPEND),
      ADDINT(SQLITE_IOCAP_SEQUENTIAL),
      END,

      DICT("mapping_sync"),
      ADDINT(SQLITE_SYNC_NORMAL),
      ADDINT(SQLITE_SYNC_FULL),
      ADDINT(SQLITE_SYNC_DATAONLY),
      END};
 
 
 for(i=0;i<sizeof(integers)/sizeof(integers[0]); i++)
   {
     const char *name=integers[i].name;
     int value=integers[i].value;
     PyObject *pyname;
     PyObject *pyvalue;

     /* should be at dict */
     if(!thedict)
       {
         assert(value==SENTINEL);
         assert(mapping_name==NULL);
         mapping_name=name;
         thedict=PyDict_New();
         continue;
       }
     /* at END? */
     if(!name)
       {
         assert(thedict);
         PyModule_AddObject(m, mapping_name, thedict);
         thedict=NULL;
         mapping_name=NULL;
         continue;
       }
     /* regular ADDINT */
     PyModule_AddIntConstant(m, name, value);
     pyname=MAKESTR(name);
     pyvalue=PyInt_FromLong(value);
     if(!pyname || !pyvalue) goto fail;
     PyDict_SetItem(thedict, pyname, pyvalue);
     PyDict_SetItem(thedict, pyvalue, pyname);
     Py_DECREF(pyname);
     Py_DECREF(pyvalue);
   }
 /* should have ended with END so thedict should be NULL */
 assert(thedict==NULL);

 if(!PyErr_Occurred())
      {
        return
#if PY_MAJOR_VERSION >= 3
          m
#endif
          ;
      }

 fail:
    Py_XDECREF(m);
    return 
#if PY_MAJOR_VERSION >= 3
          NULL
#endif
          ;
}

#ifdef APSW_TESTFIXTURES
static int
APSW_Should_Fault(const char *name)
{
  PyGILState_STATE gilstate;
  PyObject *faultdict=NULL, *truthval=NULL, *value=NULL;
  int res=0;

  gilstate=PyGILState_Ensure();

  if(!PyObject_HasAttrString(apswmodule, "faultdict"))
    PyObject_SetAttrString(apswmodule, "faultdict", PyDict_New());

  value=MAKESTR(name);
  
  faultdict=PyObject_GetAttrString(apswmodule, "faultdict");
  
  truthval=PyDict_GetItem(faultdict, value);
  if(!truthval)
    goto finally;

  /* set false if present - one shot firing */
  PyDict_SetItem(faultdict, value, Py_False);
  res=PyObject_IsTrue(truthval);

 finally:
  Py_XDECREF(value);
  Py_XDECREF(faultdict);

  PyGILState_Release(gilstate);
  return res;
}
#endif