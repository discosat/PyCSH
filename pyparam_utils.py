""" This module contains utilities for importing, initializing and using the libparam Python bindings module. """
# Without special care, as taken by this module, users themselves will be responsible for calling ._init()
# on the libparam bindings module if importing and using it manually.

from __future__ import annotations

import sys as _sys
from typing import Callable as _Callable
from builddir import libparam_py3  # Actual bindings module.
from importlib import import_module as _import_module
from contextlib import contextmanager as _contextmanager

try:  # Importing .pyi file for typehinting.
    import libparam_py3
except ModuleNotFoundError: pass

_libparam_typehint = libparam_py3


def Bindings(csp_version: int = ..., csp_hostname: str = ..., csp_model: str = ...,
             use_prometheus: int = ..., rtable: str = ..., yamlname: str = ..., dfl_addr: int = ..., quiet: int = ...,
             *, module_name: str = None) -> _libparam_typehint:
    """
    Imports and customizes a new instance of the libparam bindings module based on the provided arguments.

    :param csp_version: Which CSP version to use in the module.
    :param csp_hostname: Which CSP hostname to use in the module.
    :param csp_model: Which CSP model to use in the module.
    :param module_name: Optional alternative name of the module to import.
    :param quiet: Send output from C to /dev/null.
    :return: An instance of the libparam binded module on which operations may be performed.
    """

    init_dict = {key: value for key, value in
                 {
                     'csp_version': csp_version,
                     'csp_hostname': csp_hostname,
                     'csp_model': csp_model,
                     'use_prometheus': use_prometheus,
                     'rtable': rtable,
                     'yamlname': yamlname,
                     'dfl_addr': dfl_addr,
                     'quiet': quiet,
                 }.items()
                 if value is not ...}

    if not module_name:
        from builddir import libparam_py3 # A previous import might be reused if not deleted beforehand.
    else:
        libparam_py3 = _import_module(module_name)

    # Initialize this instance of the module with the provided settings.
    libparam_py3.param_init(**init_dict)

    return libparam_py3


class ParamMapping:
    """
    This class contains mappings to parameters through their IDs.
    Using this class; it's possible to retrieve parameters without specifying neither their name or ID, as such:
        ParamMapping().CSP_RTABLE(3) -> Returns a Python Parameter of CSP_RTABLE (provided that it is available).

    "ParamMapping()" creates a required instance of the class (a module may be specified here).

    ".CSP_RTABLE" returns a lazy constructor for a Python Parameter object
    (such that *args and **kwargs may be received and sent along).
    """
    # TODO Kevin: PyCharm fails to read the annotated typehints for attributes, and therefore thinks that they are ints.

    _param_constructor_typehint = _Callable[[int], _libparam_typehint.Parameter]

    CSP_RTABLE: _param_constructor_typehint = 12
    TFETCH_PRIMARY: _param_constructor_typehint = 41
    TFETCH_SECONDARY: _param_constructor_typehint = 42
    TFETCH_TIMEOUT: _param_constructor_typehint = 43
    TFETCH_SYNCED: _param_constructor_typehint = 44
    TFETCH_ERRORS: _param_constructor_typehint = 45
    TFETCH_LAST: _param_constructor_typehint = 46

    CSP_DEBUG: _param_constructor_typehint = 50

    COLLECTOR_CNFSTR: _param_constructor_typehint = 200
    COLLECTOR_RUN: _param_constructor_typehint = 201
    COLLECTOR_VERBOSE: _param_constructor_typehint = 202

    CRYPTO_KEY_PUBLIC: _param_constructor_typehint = 150
    CRYPTO_KEY_SECRET: _param_constructor_typehint = 151
    CRYPTO_KEY_REMOTE: _param_constructor_typehint = 152
    CRYPTO_NONCE_RX: _param_constructor_typehint = 153
    CRYPTO_NONCE_TX: _param_constructor_typehint = 154

    CRYPTO_FAUL_AUTH_COUNT: _param_constructor_typehint = 156
    CRYPTO_FAUL_NONCE_COUNT: _param_constructor_typehint = 157

    def __init__(self, module: _libparam_typehint = libparam_py3) -> None:
        super().__init__()
        self._module = module

    def __getattribute__(self, name: str) -> _Callable[[int], _libparam_typehint.Parameter]:

        paramid = super().__getattribute__(name)
        module = super().__getattribute__('_module')

        def param_constructor(*args, **kwargs) -> _libparam_typehint.Parameter:
            return module.Parameter(paramid, *args, **kwargs)

        return param_constructor


@_contextmanager
def temp_autosend_value(value: int = 0, module: _libparam_typehint = libparam_py3) -> _libparam_typehint:
    """
    Temporarily sets autosend to the provided value for the duration of the context manager block,
    to (for example) ensure that assignments are queued, and retrievals use cached values.

    :param value: The desired value of autosend, during the with block.
    :param module: Libparam bindings module instance to use.
    :return: The specified module.
    """
    original_autosend = module.autosend()
    try:
        module.autosend(value)
        yield module
    finally:
        module.autosend(original_autosend)


# class QueuedActions:
#
#     module: _libparam_typehint = None
#     original_autosend: int = None
#
#     def __init__(self, module: _libparam_typehint = libparam_py3) -> None:
#         self.module = module
#         self.original_autosend = module.autosend()
#         super().__init__()
#
#     def __enter__(self):
#         self.module.autosend(0)
#         return self
#
#     def __exit__(self, exc_type, exc_val, exc_tb):
#         self.module.autosend(self.original_autosend)
