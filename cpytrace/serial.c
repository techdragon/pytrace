#include <pthread.h>
#include "serial.h"
#include "write.h"
#include "defs.h"

#define PYPRINT(obj) PyObject_Print(obj, stdout, Py_PRINT_RAW);

static Record *record; // pre-allocate a single record to be re-used
static Argument **arguments; // pre-allocate maximum number of arguments
static unsigned char *record_buf;
static pthread_key_t depth_key;

int init_serialize(void) {
  if (0 != pthread_key_create(&depth_key, NULL)) {
    return -1;
  }
  record_buf = malloc(MAX_RECORD_SIZE);
  record = malloc(sizeof(Record)); // TODO: check malloc retval
  record__init(record);
  arguments = malloc(sizeof(Argument*) * MAX_ARGS); // TODO: check malloc retval
  record->arguments = arguments;
  int i;
  for (i = 0; i < MAX_ARGS; i++) {
    arguments[i] = malloc(sizeof(Argument));
    argument__init(arguments[i]);
  }
  return init_writer();
}

inline const char *pyobj_to_cstr(PyObject *obj) {
  PyObject *string = PyObject_Repr(obj);
  if (NULL == string) {
    return "REPR FAILED";
  }
  return PyString_AsString(string);
}

inline static double floattime()
{
  struct timeval t;
  gettimeofday(&t, NULL);
  return (double) t.tv_sec + t.tv_usec * 0.000001;
}

inline static int get_depth() {
  return (int) pthread_getspecific(depth_key); // if called before inc/dec will return NULL -> 0
}

inline static void increment_depth() {
  pthread_setspecific(depth_key, get_depth() + 1);
}  

inline static void decrement_depth() {
  pthread_setspecific(depth_key, get_depth() - 1);
}

void handle_trace(PyFrameObject *frame, Record__RecordType record_type, int n_arguments) 
{
  record->type = record_type;
  record->n_arguments = n_arguments;
  record->time = floattime();
  record->tid = (unsigned int) pthread_self();
  record->function = PyString_AsString(frame->f_code->co_name);
  record->module = PyString_AsString(frame->f_code->co_filename);
  record->depth = get_depth();
  record__pack(record, record_buf);
  write_record(record_buf, record__get_packed_size(record));
}

void handle_call(PyFrameObject *frame) {  
  PyObject *name, *value;
  int i;
  increment_depth();
  for (i = 0; i < MIN(PyTuple_GET_SIZE(frame->f_code->co_varnames), MAX_ARGS); i++) {
    name = PyTuple_GetItem(frame->f_code->co_varnames, i);
    arguments[i]->name = PyString_AsString(name);
    if (NULL == frame->f_locals) {
      value = frame->f_localsplus[i];
    } else {
      value = PyDict_GetItem(frame->f_locals, name);
    }
    arguments[i]->type = pyobj_to_cstr(value->ob_type);
    arguments[i]->value = pyobj_to_cstr(value);
  }
  handle_trace(frame, RECORD__RECORD_TYPE__CALL, i);
}

inline void handle_return(PyFrameObject *frame, PyObject *value) {
  decrement_depth();
  arguments[0]->name = "return_value";
  if (NULL == value) {
    value = Py_None;
  }
  arguments[0]->type = pyobj_to_cstr(value->ob_type);
  arguments[0]->value = pyobj_to_cstr(value);
  handle_trace(frame, RECORD__RECORD_TYPE__RETURN, 1);
}
    
inline void handle_exception(PyFrameObject *frame, PyObject *exc_info) {
  arguments[0]->name = "exception";
  arguments[0]->type = pyobj_to_cstr(PyTuple_GET_ITEM(exc_info, 1));
  arguments[0]->value = pyobj_to_cstr(PyTuple_GET_ITEM(exc_info, 2));
  handle_trace(frame, RECORD__RECORD_TYPE__EXCEPTION, 1);
}
