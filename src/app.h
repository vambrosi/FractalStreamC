/* app.h - the GUI application. */
#ifndef FS_APP_H
#define FS_APP_H

/* Open the GUI.  `script_path` may be NULL: the editor then starts with an example. */
int app_run(const char *script_path);

/* After gfx_init: choose how deep zooms (double-single shaders) keep their float operations exact, checking the
   faster way on the GPU (see codegen_set_float_controls).  Returns whether the faster way is used. */
bool app_init_exact_floats(void);

#endif
