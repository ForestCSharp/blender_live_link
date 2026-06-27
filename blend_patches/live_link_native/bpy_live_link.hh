/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <Python.h>

namespace blender {

PyObject *BPY_live_link_make_update(PyObject *objects,
                                    PyObject *deleted_object_uids,
                                    PyObject *dependency_graph,
                                    bool reset,
                                    const char *update_reason,
                                    int sequence);

}  // namespace blender
