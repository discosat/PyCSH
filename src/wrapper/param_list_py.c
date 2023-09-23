/*
 * param_list_py.c
 *
 * Wrappers for lib/param/src/param/list/param_list_slash.c
 *
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <param/param_string.h>

#include "../pycsh.h"
#include "../utils.h"
#include "../parameter/parameter.h"
#include "../parameter/pythonparameter.h"

#include "param_list_py.h"

PyObject * pycsh_param_list(PyObject * self, PyObject * args, PyObject * kwds) {

    int node = pycsh_dfl_node;
    int verbosity = 1;
    char * maskstr = NULL;
    char * globstr = NULL;

    static char *kwlist[] = {"node", "verbose", "mask", "globstr", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iiss", kwlist, &node, &verbosity, &maskstr, &globstr)) {
        return NULL;
    }

    /* Interpret maskstring */
    uint32_t mask = 0xFFFFFFFF;
    if (maskstr != NULL) {
        mask = param_maskstr_to_mask(maskstr);
    }

    param_list_print(mask, node, globstr, verbosity);

    return pycsh_util_parameter_list();
}

PyObject * pycsh_param_list_download(PyObject * self, PyObject * args, PyObject * kwds) {

    CSP_INIT_CHECK()

    unsigned int node = pycsh_dfl_node;
    unsigned int timeout = pycsh_dfl_timeout;
    unsigned int version = 2;
    int include_remotes = 0;

    static char *kwlist[] = {"node", "timeout", "version", "remotes", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|IIII", kwlist, &node, &timeout, &version, &include_remotes))
        return NULL;  // TypeError is thrown

    {  /* Allow threads during list_download() */
        int list_download_res;
        Py_BEGIN_ALLOW_THREADS;
        list_download_res = param_list_download(node, timeout, version, include_remotes);
        Py_END_ALLOW_THREADS;
        // TODO Kevin: Downloading parameters with an incorrect version, can lead to a segmentation fault.
        //	Had it been easier to detect when an incorrect version is used, we would've raised an exception instead.
        if (list_download_res < 1) {  // We assume a connection error has occurred if we don't receive any parameters.
            PyErr_SetString(PyExc_ConnectionError, "No response.");
            return NULL;
        }
    }

#if 0
    /* Despite the nastiness, we reallocate the downloaded parameters, such that they become embedded in a ParameterObject.
        Embedding the parameters allows us to create new parameters from Python, without the need for a lookup table for Python callbacks. */
    param_list_iterator i = {};
    param_t * iter_param = param_list_iterate(&i);

    while (iter_param) {

        if (i.phase == 0) {
            iter_param = param_list_iterate(&i);
            continue;  // We cannot reallocate static parameters.
            /* TODO Kevin: We could, however, consider reusing their .addr for our new parameter.
                But not here in .list_download() */
        }

        param_t * param = iter_param;  // Free the current parameter after we have used it to iterate.
        iter_param = param_list_iterate(&i);

        if (Parameter_wraps_param(param))
            continue;  // This parameter doesn't need reallocation.

        ParameterObject * pyparam = (ParameterObject *)_pycsh_Parameter_from_param(&ParameterType, param, NULL, INT_MIN, pycsh_dfl_timeout, 1, 2);

        if (pyparam == NULL) {
            PyErr_SetString(PyExc_MemoryError, "Failed to create ParameterObject for downloaded parameter. Parameter list may be corrupted.");
            return NULL;
        }

        // Using param_list_remove_specific() means we iterate thrice, but it is simpler.
        param_list_remove_specific(param, 0, 1);

        param_list_add(&pyparam->param);
    }
#endif

    return pycsh_util_parameter_list();

}

PyObject * pycsh_param_list_forget(PyObject * self, PyObject * args, PyObject * kwds) {

    int node = pycsh_dfl_node;
    int verbose = 1;

    static char *kwlist[] = {"node", "verbose", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ii", kwlist, &node, &verbose))
        return NULL;  // TypeError is thrown

    int res = param_list_remove(node, verbose);
    printf("Removed %i parameters\n", res);
    return Py_BuildValue("i", res);;
}

PyObject * pycsh_param_list_save(PyObject * self, PyObject * args, PyObject * kwds) {

    char * filename = NULL;
    int node = pycsh_dfl_node;
    int include_node = 1;  // Make node optional, as to support adding to env node


    static char *kwlist[] = {"filename", "node", "include_node", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|sip", kwlist, &filename, &node, &include_node))
        return NULL;  // TypeError is thrown

    FILE * out = stdout;

    if (filename) {
	    FILE * fd = fopen(filename, "w");
        if (fd) {
            out = fd;
            printf("Writing to file %s\n", filename);
        }
    }

    // TODO Kevin: Ideally we would extract most of this to a separate function in libparam,
    //  so we can keep it more DRY.
    param_t * param;
	param_list_iterator i = {};
	while ((param = param_list_iterate(&i)) != NULL) {

        if ((node >= 0) && (param->node != node)) {
			continue;
		}

        fprintf(out, "list add ");
        if (param->array_size > 1) {
            fprintf(out, "-a %u ", param->array_size);
        }
        if ((param->docstr != NULL) && (strlen(param->docstr) > 0)) {
            fprintf(out, "-c \"%s\" ", param->docstr);
        }
        if ((param->unit != NULL) && (strlen(param->unit) > 0)) {
            fprintf(out, "-u \"%s\" ", param->unit);
        }
        if (param->node != 0 && include_node) {
            fprintf(out, "-n %u ", param->node);
        }
        
		if (param->mask > 0) {
			unsigned int mask = param->mask;

			fprintf(out, "-m \"");

			if (mask & PM_READONLY) {
				mask &= ~ PM_READONLY;
				fprintf(out, "r");
			}

			if (mask & PM_REMOTE) {
				mask &= ~ PM_REMOTE;
				fprintf(out, "R");
			}

			if (mask & PM_CONF) {
				mask &= ~ PM_CONF;
				fprintf(out, "c");
			}

			if (mask & PM_TELEM) {
				mask &= ~ PM_TELEM;
				fprintf(out, "t");
			}

			if (mask & PM_HWREG) {
				mask &= ~ PM_HWREG;
				fprintf(out, "h");
			}

			if (mask & PM_ERRCNT) {
				mask &= ~ PM_ERRCNT;
				fprintf(out, "e");
			}

			if (mask & PM_SYSINFO) {
				mask &= ~ PM_SYSINFO;
				fprintf(out, "i");
			}

			if (mask & PM_SYSCONF) {
				mask &= ~ PM_SYSCONF;
				fprintf(out, "C");
			}

			if (mask & PM_WDT) {
				mask &= ~ PM_WDT;
				fprintf(out, "w");
			}

			if (mask & PM_DEBUG) {
				mask &= ~ PM_DEBUG;
				fprintf(out, "d");
			}

			if (mask & PM_ATOMIC_WRITE) {
				mask &= ~ PM_ATOMIC_WRITE;
				fprintf(out, "o");
			}

			if (mask & PM_CALIB) {
				mask &= ~ PM_CALIB;
				fprintf(out, "q");
			}

            switch(mask & PM_PRIO_MASK) {
                case PM_PRIO1: fprintf(out, "1"); mask &= ~ PM_PRIO_MASK; break;
                case PM_PRIO2: fprintf(out, "2"); mask &= ~ PM_PRIO_MASK; break;
                case PM_PRIO3: fprintf(out, "3"); mask &= ~ PM_PRIO_MASK; break;				
			}

			//if (mask)
			//	fprintf(out, "+%x", mask);

            fprintf(out, "\" ");

		}
		
        fprintf(out, "%s %u ", param->name, param->id);

        char typestr[10];
        param_type_str(param->type, typestr, 10);
        fprintf(out, "%s\n", typestr);

	}

    if (out != stdout) {
        fflush(out);
        fclose(out);
    }

    Py_RETURN_NONE;
}
