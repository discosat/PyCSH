/*
 * pycsh.c
 *
 * Setup and initialization for the PyCSH module.
 *
 *  Created on: Apr 28, 2022
 *      Author: Kevin Wallentin Carlsen
 */

// It is recommended to always define PY_SSIZE_T_CLEAN before including Python.h
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "structmember.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/utsname.h>

#include <param/param.h>
#ifdef PARAM_HAVE_COMMANDS
#include <param/param_commands.h>
#endif
#ifdef PARAM_HAVE_SCHEDULER
#include <param/param_scheduler.h>
#endif

#include <vmem/vmem_file.h>

#include <csp/csp.h>
#include <csp/csp_yaml.h>
#include <csp/csp_cmp.h>
#include <pthread.h>
#include <csp/interfaces/csp_if_can.h>
#include <csp/interfaces/csp_if_kiss.h>
#include <csp/interfaces/csp_if_udp.h>
#include <csp/interfaces/csp_if_zmqhub.h>
#include <csp/drivers/usart.h>
#include <csp/drivers/can_socketcan.h>

#include <sys/types.h>

#include "csh/prometheus.h"
#include "csh/param_sniffer.h"

#include "utils.h"

#include "parameter/parameter.h"
#include "parameter/parameterarray.h"
#include "parameter/pythonparameter.h"
#include "parameter/parameterlist.h"

#include "wrapper/py_csp.h"
#include "wrapper/param_py.h"
#include "wrapper/dflopt_py.h"
#include "wrapper/spaceboot_py.h"
#include "wrapper/csp_init_py.h"
#include "wrapper/param_list_py.h"
#include "wrapper/vmem_client_py.h"


VMEM_DEFINE_FILE(col, "col", "colcnf.vmem", 120);
#ifdef PARAM_HAVE_COMMANDS
VMEM_DEFINE_FILE(commands, "cmd", "commands.vmem", 2048);
#endif
#ifdef PARAM_HAVE_SCHEDULER
VMEM_DEFINE_FILE(schedule, "sch", "schedule.vmem", 2048);
#endif
VMEM_DEFINE_FILE(dummy, "dummy", "dummy.txt", 1000000);

// #define PARAMID_CSP_RTABLE					 12
// PARAM_DEFINE_STATIC_VMEM(PARAMID_CSP_RTABLE,      csp_rtable,        PARAM_TYPE_STRING, 64, 0, PM_SYSCONF, NULL, "", csp, 0, NULL);


// We include this parameter when testing the behavior of arrays, as none would exist otherwise.
uint8_t _test_array[] = {1,2,3,4,5,6,7,8};
PARAM_DEFINE_STATIC_RAM(1001, test_array_param,          PARAM_TYPE_UINT8,  8, sizeof(uint8_t),  PM_DEBUG, NULL, "", _test_array, "Parameter to use when testing arrays.");

static char _test_str[80];
PARAM_DEFINE_STATIC_RAM(1002, test_str,          PARAM_TYPE_STRING,  80, 1,  PM_DEBUG, NULL, "", _test_str, "Parameter to use when testing strings");


// Keep track of whether init has been run,
// to prevent unexpected behavior from running relevant functions first.
uint8_t _csp_initialized = 0;

uint8_t csp_initialized() {
	return _csp_initialized;
}

unsigned int pycsh_dfl_node = 0;
unsigned int pycsh_dfl_timeout = 1000;

uint64_t clock_get_nsec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1E9 + ts.tv_nsec;
}

void * onehz_task(void * param) {
	while(1) {
#ifdef PARAM_HAVE_SCHEDULER
		csp_timestamp_t scheduler_time = {};
        csp_clock_get_time(&scheduler_time);
        param_schedule_server_update(scheduler_time.tv_sec * 1E9 + scheduler_time.tv_nsec);
#endif
		// Py_BEGIN_ALLOW_THREADS;
		sleep(1);
		// Py_END_ALLOW_THREADS;
	}
	return NULL;
}

// TODO Kevin: It's probably not safe to call this function consecutively with the same std_stream or stream_buf.
static int _handle_stream(PyObject * stream_identifier, FILE **std_stream, FILE *stream_buf) {
	if (stream_identifier == NULL)
		return 0;

	if (PyLong_Check(stream_identifier)) {
		long int val = PyLong_AsLong(stream_identifier);
		switch (val) {
			case -2:  // STDOUT
				break;  // Default behavior is correct, don't do anything.
			case -3:  // DEVNULL
				// fclose(stdout);
				if ((stream_buf = fopen("/dev/null", "w")) == NULL) {
					char buf[150];
					snprintf(buf, 150, "Impossible error! Can't open /dev/null: %s\n", strerror(errno));
					PyErr_SetString(PyExc_IOError, buf);
					return -1;
				}
				*std_stream = stream_buf;
				break;
			default:
				PyErr_SetString(PyExc_ValueError, "Argument should be either -2 for subprocess.STDOUT, -3 for subprocess.DEVNULL or a string to a file.");
				return -2;
		}
	} else if (PyUnicode_Check(stream_identifier)) {
		const char *filename = PyUnicode_AsUTF8(stream_identifier);

		if (stream_buf != NULL)
			fclose(stream_buf);

		if ((stream_buf = freopen(filename, "w+", *std_stream)) == NULL) {
			char buf[30 + strlen(filename)];
			sprintf(buf, "Failed to open file: %s", filename);
			PyErr_SetString(PyExc_IOError, buf);
			return -3;
		}
		// std_stream = stream_buf;
	} else {
		PyErr_SetString(PyExc_TypeError, "Argument should be either -2 for subprocess.STDOUT, -3 for subprocess.DEVNULL or a string to a file.");
		return -4;
	}
	return 0;
}

static PyObject * pycsh_init(PyObject * self, PyObject * args, PyObject *kwds) {

	if (_csp_initialized) {
		PyErr_SetString(PyExc_RuntimeError,
			"Cannot initialize multiple instances of libparam bindings. Please use a previous binding.");
		return NULL;
	}

	static char *kwlist[] = {
		"quiet", "stdout", "stderr", NULL,
	};

	int quiet = 0;
	PyObject *csh_stdout = NULL;
	PyObject *csh_stderr = NULL;
	

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iOO:init", kwlist, &quiet, &csh_stdout, &csh_stderr))
		return NULL;  // TypeError is thrown

	// TODO Kevin: Support reassigning streams through module function or global.
	static FILE * temp_stdout_fd = NULL;
	static FILE * temp_stderr_fd = NULL;

	if (quiet) {
		if ((temp_stdout_fd = fopen("/dev/null", "w")) == NULL) {
			fprintf(stderr, "Impossible error! Can't open /dev/null: %s\n", strerror(errno));
			exit(1);
		}
		stdout = temp_stdout_fd;
	}
	else if (
		_handle_stream(csh_stdout, &stdout, temp_stdout_fd) != 0 ||
		_handle_stream(csh_stderr, &stderr, temp_stderr_fd) != 0
	) {
		return NULL;
	}

	srand(time(NULL));

	void serial_init(void);
	serial_init();

	/* TODO Kevin: If we include slash as a submodule, we should init it here. */

#ifdef PARAM_HAVE_COMMANDS
	vmem_file_init(&vmem_commands);
	param_command_server_init();
#endif
#ifdef PARAM_HAVE_SCHEDULER
	param_schedule_server_init();
#endif

	vmem_file_init(&vmem_dummy);

	static pthread_t onehz_handle;
	pthread_create(&onehz_handle, NULL, &onehz_task, NULL);

	/* TODO Kevin: If we include slash as a submodule, we should run the init file here. */
	
	_csp_initialized = 1;
	Py_RETURN_NONE;

}


static PyMethodDef methods[] = {

	/* Converted CSH commands from libparam/src/param/param_slash.c */
	{"get", 		(PyCFunction)pycsh_param_get, 	METH_VARARGS | METH_KEYWORDS, "Set the value of a parameter."},
	{"set", 		(PyCFunction)pycsh_param_set, 	METH_VARARGS | METH_KEYWORDS, "Get the value of a parameter."},
	// {"push", 		(PyCFunction)pycsh_param_push,	METH_VARARGS | METH_KEYWORDS, "Push the current queue."},
	{"pull", 		(PyCFunction)pycsh_param_pull,	METH_VARARGS | METH_KEYWORDS, "Pull all or a specific set of parameters."},
	{"cmd_done", 	pycsh_param_cmd_done, 			METH_NOARGS, 				  "Clears the queue."},
	{"cmd_new", 	(PyCFunction)pycsh_param_cmd_new,METH_VARARGS | METH_KEYWORDS, "Create a new command"},
	{"node", 		pycsh_slash_node, 			  	METH_VARARGS, 				  "Used to get or change the default node."},
	{"timeout", 	pycsh_slash_timeout, 			METH_VARARGS, 		  		  "Used to get or change the default timeout."},
	{"queue", 		pycsh_param_cmd,			  	METH_NOARGS, 				  "Print the current command."},

	/* Converted CSH commands from libparam/src/param/list/param_list_slash.c */
	{"list", 		(PyCFunction)pycsh_param_list,	METH_VARARGS | METH_KEYWORDS, "List all known parameters."},
	{"list_download", (PyCFunction)pycsh_param_list_download, METH_VARARGS | METH_KEYWORDS, "Download all parameters on the specified node."},
	{"list_forget", (PyCFunction)pycsh_param_list_forget, METH_VARARGS | METH_KEYWORDS, "Remove remote parameters, matching the provided arguments, from the global list."},
	// {"list_save", 	pycsh_param_list_save, 		  	METH_VARARGS, 				  "Save a list of parameters to a file."},
	// {"list_load", 	pycsh_param_list_load, 		  	METH_VARARGS, 				  "Load a list of parameters from a file."},

	/* Converted CSH commands from csh/src/slash_csp.c */
	{"ping", 		(PyCFunction)pycsh_slash_ping, 	METH_VARARGS | METH_KEYWORDS, "Ping the specified node."},
	{"ident", 		(PyCFunction)pycsh_slash_ident,	METH_VARARGS | METH_KEYWORDS, "Print the identity of the specified node."},
	{"reboot", 		pycsh_slash_reboot, 			 	METH_VARARGS, 				  "Reboot the specified node."},

	/* Utility functions */
	{"get_type", 	pycsh_util_get_type, 		  	METH_VARARGS, 				  "Gets the type of the specified parameter."},

	/* Converted vmem commands from libparam/src/vmem/vmem_client_slash.c */
	{"vmem", 	(PyCFunction)pycsh_param_vmem,   METH_VARARGS | METH_KEYWORDS, "Builds a string of the vmem at the specified node."},

	/* Converted program/reboot commands from csh/src/spaceboot_slash.c */
	{"switch", 	(PyCFunction)slash_csp_switch,   METH_VARARGS | METH_KEYWORDS, "Reboot into the specified firmware slot."},
	{"program", (PyCFunction)pycsh_csh_program,  METH_VARARGS | METH_KEYWORDS, "Upload new firmware to a module."},
	{"sps", 	(PyCFunction)slash_sps,   		 METH_VARARGS | METH_KEYWORDS, "Switch -> Program -> Switch"},

	/* Wrappers for src/csp_init_cmd.c */
	{"csp_init", 	(PyCFunction)pycsh_csh_csp_init,   METH_VARARGS | METH_KEYWORDS, "Initialize CSP"},
	{"csp_add_zmq", (PyCFunction)pycsh_csh_csp_ifadd_zmq,   METH_VARARGS | METH_KEYWORDS, "Add a new ZMQ interface"},
	{"csp_add_kiss",(PyCFunction)pycsh_csh_csp_ifadd_kiss,   METH_VARARGS | METH_KEYWORDS, "Add a new KISS/UART interface"},
#if (CSP_HAVE_LIBSOCKETCAN)
	{"csp_add_can", (PyCFunction)pycsh_csh_csp_ifadd_can,   METH_VARARGS | METH_KEYWORDS, "Add a new UDP interface"},
#endif
	{"csp_add_udp", (PyCFunction)pycsh_csh_csp_ifadd_udp,   METH_VARARGS | METH_KEYWORDS, "Add a new TUN interface"},
	{"csp_add_tun", (PyCFunction)pycsh_csh_csp_ifadd_tun,   METH_VARARGS | METH_KEYWORDS, "Add a new route"},

	/* Misc */
	{"init", (PyCFunction)pycsh_init, 				METH_VARARGS | METH_KEYWORDS, "Initializes the module, with the provided settings."},

	/* sentinel */
	{NULL, NULL, 0, NULL}};

static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	"pycsh",
	"Bindings primarily dedicated to the CSH shell interface commands",
	-1,
	methods,
	NULL,
	NULL,
	NULL,
	NULL};

PyMODINIT_FUNC PyInit_pycsh(void) {

	if (PyType_Ready(&ParameterType) < 0)
        return NULL;

	if (PyType_Ready(&ParameterArrayType) < 0)
        return NULL;

	if (PyType_Ready(&PythonParameterType) < 0)
        return NULL;

	ParameterListType.tp_base = &PyList_Type;
	if (PyType_Ready(&ParameterListType) < 0)
		return NULL;

	PyObject * m = PyModule_Create(&moduledef);
	if (m == NULL)
		return NULL;

	Py_INCREF(&ParameterType);
	if (PyModule_AddObject(m, "Parameter", (PyObject *) &ParameterType) < 0) {
		Py_DECREF(&ParameterType);
        Py_DECREF(m);
        return NULL;
	}

	Py_INCREF(&ParameterArrayType);
	if (PyModule_AddObject(m, "ParameterArray", (PyObject *) &ParameterArrayType) < 0) {
		Py_DECREF(&ParameterArrayType);
        Py_DECREF(m);
        return NULL;
	}

	Py_INCREF(&PythonParameterType);
	if (PyModule_AddObject(m, "PythonParameter", (PyObject *) &PythonParameterType) < 0) {
		Py_DECREF(&PythonParameterType);
        Py_DECREF(m);
        return NULL;
	}

	Py_INCREF(&ParameterListType);
	if (PyModule_AddObject(m, "ParameterList", (PyObject *)&ParameterListType) < 0) {
		Py_DECREF(&ParameterListType);
		Py_DECREF(m);
		return NULL;
	}

	return m;
}
